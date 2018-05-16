/**
 *  @file
 *  @copyright defined in evt/LICENSE.txt
 */
#include <evt/chain/block_context.hpp>
#include <evt/chain/controller.hpp>
#include <evt/chain/transaction_context.hpp>

#include <evt/chain/authority_checker.hpp>
#include <evt/chain/block_log.hpp>
#include <evt/chain/fork_database.hpp>
#include <evt/chain/token_database.hpp>

#include <evt/chain/block_summary_object.hpp>
#include <evt/chain/global_property_object.hpp>
#include <evt/chain/transaction_object.hpp>

#include <chainbase/chainbase.hpp>
#include <fc/io/json.hpp>
#include <fc/scoped_exit.hpp>

#include <evt/chain/contracts/evt_contract.hpp>

namespace evt { namespace chain {

struct pending_state {
    pending_state(database::session&& s, token_database::session&& ts)
        : _db_session(move(s))
        , _token_db_session(move(ts)) {}
    pending_state(pending_state&& ps)
        : _db_session(move(ps._db_session))
        , _token_db_session(move(ps._token_db_session)) {}

    database::session       _db_session;
    token_database::session _token_db_session;

    block_state_ptr _pending_block_state;

    vector<action_receipt> _actions;

    block_context _block_ctx;

    void
    push() {
        _db_session.push();
        _token_db_session.accept();
    }
};

namespace __internal {

auto
get_auth_checker(const evt::chain::token_database& token_db, const flat_set<public_key_type>& keys) {
    auto checker
        = make_auth_checker(keys,
                            [&](const auto& domain, const auto name, const auto& cb) {
                                token_db.read_domain(domain, [&](const auto& domain) {
                                    if(name == N(issuetoken)) {
                                        cb(domain.issue);
                                    }
                                    else if(name == N(transfer)) {
                                        cb(domain.transfer);
                                    }
                                    else if(name == N(updatedomain)) {
                                        cb(domain.manage);
                                    }
                                });
                            },
                            [&](const auto& id, const auto& cb) { token_db.read_group(id, cb); },
                            [&](const auto& domain, const auto& name, const auto& cb) {
                                if(domain == N128(account)) {
                                    token_db.read_account(name, [&](const auto& account) { cb(account.owner); });
                                }
                                else {
                                    token_db.read_token(domain, name, [&](const auto& token) { cb(token.owner); });
                                }
                            });
    return checker;
}

}  // namespace __internal

struct controller_impl {
    controller&             self;
    chainbase::database     db;
    block_log               blog;
    optional<pending_state> pending;
    block_state_ptr         head;
    fork_database           fork_db;
    token_database          token_db;
    controller::config      conf;
    bool                    replaying = false;
    abi_serializer          system_api;

    map<action_name, apply_handler> apply_handlers;

    /**
    *  Transactions that were undone by pop_block or abort_block, transactions
    *  are removed from this list if they are re-applied in other blocks. Producers
    *  can query this list when scheduling new transactions into blocks.
    */
    map<digest_type, transaction_metadata_ptr> unapplied_transactions;

    void
    pop_block() {
        auto prev = fork_db.get_block(head->header.previous);
        FC_ASSERT(prev, "attempt to pop beyond last irreversible block");
        for(const auto& t : head->trxs)
            unapplied_transactions[t->signed_id] = t;
        head = prev;
        db.undo();
        token_db.rollback_to_latest_savepoint();
    }

    void
    set_apply_handler(action_name action, apply_handler v) {
        apply_handlers[action] = v;
    }

    controller_impl(const controller::config& cfg, controller& s)
        : self(s)
        , db(cfg.shared_memory_dir,
             cfg.read_only ? database::read_only : database::read_write,
             cfg.shared_memory_size)
        , blog(cfg.block_log_dir)
        , fork_db(cfg.shared_memory_dir)
        , token_db(cfg.tokendb_dir)
        , conf(cfg)
        , system_api(contracts::evt_contract_abi()) {
#define SET_APP_HANDLER(action) \
    set_apply_handler(#action, &BOOST_PP_CAT(contracts::apply_evt, BOOST_PP_CAT(_, action)))

        SET_APP_HANDLER(newdomain);
        SET_APP_HANDLER(issuetoken);
        SET_APP_HANDLER(transfer);
        SET_APP_HANDLER(newgroup);
        SET_APP_HANDLER(updategroup);
        SET_APP_HANDLER(updatedomain);
        SET_APP_HANDLER(newaccount);
        SET_APP_HANDLER(updateowner);
        SET_APP_HANDLER(transferevt);

        fork_db.irreversible.connect([&](auto b) {
            on_irreversible(b);
        });
    }

    /**
    *  Plugins / observers listening to signals emited (such as accepted_transaction) might trigger
    *  errors and throw exceptions. Unless those exceptions are caught it could impact consensus and/or
    *  cause a node to fork.
    *
    *  If it is ever desirable to let a signal handler bubble an exception out of this method
    *  a full audit of its uses needs to be undertaken.
    *
    */
    template <typename Signal, typename Arg>
    void
    emit(const Signal& s, Arg&& a) {
        try {
            s(std::forward<Arg>(a));
        }
        catch(...) {
            elog("signal handler threw exception");
        }
    }

    void
    on_irreversible(const block_state_ptr& s) {
        if(!blog.head())
            blog.read_head();

        const auto& log_head = blog.head();
        FC_ASSERT(log_head);
        auto lh_block_num = log_head->block_num();

        if(s->block_num - 1 == lh_block_num) {
            FC_ASSERT(s->block->previous == log_head->id(), "irreversible doesn't link to block log head");
            blog.append(s->block);
        }
        else if(s->block_num - 1 > lh_block_num) {
            wlog("skipped blocks...");
            edump((s->block_num)(log_head->block_num()));
            if(s->block_num == log_head->block_num()) {
                FC_ASSERT(s->id == log_head->id(), "", ("s->id", s->id)("hid", log_head->id()));
            }
        }
        emit(self.irreversible_block, s);
        db.commit(s->block_num);
        token_db.pop_savepoints(s->block_num);
    }

    void
    init() {
        /**
      *  The fork database needs an initial block_state to be set before
      *  it can accept any new blocks. This initial block state can be found
      *  in the database (whose head block state should be irreversible) or
      *  it would be the genesis state.
      */
        if(!head) {
            initialize_fork_db();  // set head to genesis state
        }

        while(db.revision() > head->block_num) {
            wlog("warning database revision greater than head block, undoing pending changes");
            db.undo();
        }

        FC_ASSERT(db.revision() == head->block_num, "fork database is inconsistent with shared memory",
                  ("db", db.revision())("head", head->block_num));

        /**
       * The undoable state contains state transitions from blocks
       * in the fork database that could be reversed. Because this
       * is a new startup and the fork database is empty, we must
       * unwind that pending state. This state will be regenerated
       * when we catch up to the head block later.
       */
        //clear_all_undo();
    }

    ~controller_impl() {
        pending.reset();
        fork_db.close();

        edump((db.revision())(head->block_num)(blog.read_head()->block_num()));

        db.flush();
    }

    void
    add_indices() {
        db.add_index<global_property_multi_index>();
        db.add_index<dynamic_global_property_multi_index>();
        db.add_index<block_summary_multi_index>();
        db.add_index<transaction_multi_index>();
    }

    /**
    *  Sets fork database head to the genesis state.
    */
    void
    initialize_fork_db() {
        wlog(" Initializing new blockchain with genesis state                  ");
        producer_schedule_type initial_schedule{0, {{N(evt), conf.genesis.initial_key}}};

        block_header_state genheader;
        genheader.active_schedule       = initial_schedule;
        genheader.pending_schedule      = initial_schedule;
        genheader.pending_schedule_hash = fc::sha256::hash(initial_schedule);
        genheader.header.timestamp      = conf.genesis.initial_timestamp;
        genheader.header.action_mroot   = conf.genesis.compute_chain_id();
        genheader.id                    = genheader.header.id();
        genheader.block_num             = genheader.header.block_num();

        head        = std::make_shared<block_state>(genheader);
        head->block = std::make_shared<signed_block>(genheader.header);
        fork_db.set(head);
        db.set_revision(head->block_num);

        initialize_database();

        auto end = blog.read_head();
        if(end && end->block_num() > 1) {
            replaying = true;
            ilog("existing block log, attempting to replay ${n} blocks", ("n", end->block_num()));

            auto start = fc::time_point::now();
            while(auto next = blog.read_block_by_num(head->block_num + 1)) {
                self.push_block(next, true);
                if(next->block_num() % 100 == 0) {
                    std::cerr << std::setw(10) << next->block_num() << " of " << end->block_num() << "\r";
                }
            }
            std::cerr << "\n";
            auto end = fc::time_point::now();
            ilog("replayed blocks in ${n} seconds, ${spb} spb", ("n", (end - start).count() / 1000000.0)("spb", ((end - start).count() / 1000000.0) / head->block_num));
            replaying = false;
        }
        else if(!end) {
            blog.append(head->block);
        }
    }

    void
    initialize_database() {
        // Initialize block summary index
        for(int i = 0; i < 0x10000; i++)
            db.create<block_summary_object>([&](block_summary_object&) {});

        const auto& tapos_block_summary = db.get<block_summary_object>(1);
        db.modify(tapos_block_summary, [&](auto& bs) {
            bs.block_id = head->id;
        });

        db.create<global_property_object>([&](auto& gpo) {
            gpo.configuration = conf.genesis.initial_configuration;
        });
        db.create<dynamic_global_property_object>([](auto&) {});
    }

    void
    commit_block(bool add_to_fork_db) {
        if(add_to_fork_db) {
            pending->_pending_block_state->validated = true;
            auto new_bsp                             = fork_db.add(pending->_pending_block_state);
            emit(self.accepted_block_header, pending->_pending_block_state);
            head = fork_db.head();
            FC_ASSERT(new_bsp == head, "committed block did not become the new head in fork database");
        }

        //    ilog((fc::json::to_pretty_string(*pending->_pending_block_state->block)));
        emit(self.accepted_block, pending->_pending_block_state);
        pending->push();
        pending.reset();

        self.log_irreversible_blocks();
    }

    // The returned scoped_exit should not exceed the lifetime of the pending which existed when make_block_restore_point was called.
    fc::scoped_exit<std::function<void()>>
    make_block_restore_point() {
        auto orig_block_transactions_size = pending->_pending_block_state->block->transactions.size();
        auto orig_state_transactions_size = pending->_pending_block_state->trxs.size();
        auto orig_state_actions_size      = pending->_actions.size();

        std::function<void()> callback = [this,
                                          orig_block_transactions_size,
                                          orig_state_transactions_size,
                                          orig_state_actions_size]() {
            pending->_pending_block_state->block->transactions.resize(orig_block_transactions_size);
            pending->_pending_block_state->trxs.resize(orig_state_transactions_size);
            pending->_actions.resize(orig_state_actions_size);
        };

        return fc::make_scoped_exit(std::move(callback));
    }

    /**
    *  Adds the transaction receipt to the pending block and returns it.
    */
    template <typename T>
    const transaction_receipt&
    push_receipt(const T& trx, transaction_receipt_header::status_enum status) {
        pending->_pending_block_state->block->transactions.emplace_back(trx);
        transaction_receipt& r = pending->_pending_block_state->block->transactions.back();
        r.status               = status;
        return r;
    }

    bool
    failure_is_subjective( const fc::exception& e ) {
        auto code = e.code();
        return (code == deadline_exception::code_value);
    }

    /**
    *  This is the entry point for new transactions to the block state. It will check authorization and
    *  determine whether to execute it now or to delay it. Lastly it inserts a transaction receipt into
    *  the pending block.
    */
    transaction_trace_ptr
    push_transaction(const transaction_metadata_ptr& trx,
                     fc::time_point                  deadline,
                     bool                            implicit) {
        FC_ASSERT(deadline != fc::time_point(), "deadline cannot be uninitialized");

        transaction_trace_ptr trace;
        try {
            transaction_context trx_context(self, trx->trx, trx->id);
            trx_context.deadline = deadline;
            trace                = trx_context.trace;
            try {
                if(implicit) {
                    trx_context.init_for_implicit_trx();
                }
                else {
                    trx_context.init_for_input_trx(trx->trx.signatures.size());
                }

                if(!implicit) {
                    auto checker = __internal::get_auth_checker(token_db, trx->recover_keys());
                    for(const auto& act : trx->trx.actions) {
                        EVT_ASSERT(checker.satisfied(act), tx_missing_sigs,
                                   "${name} action in domain: ${domain} with key: ${key} authorized failed",
                                   ("domain", act.domain)("key", act.key)("name", act.name));
                    }
                }

                trx_context.exec();
                trx_context.finalize();  // Automatically rounds up network and CPU usage in trace and bills payers if successful

                auto restore = make_block_restore_point();

                if(!implicit) {
                    trace->receipt = push_receipt(trx->packed_trx, transaction_receipt::executed);
                    pending->_pending_block_state->trxs.emplace_back(trx);
                }
                else {
                    transaction_receipt_header r;
                    r.status       = transaction_receipt::executed;
                    trace->receipt = r;
                }

                fc::move_append(pending->_actions, move(trx_context.executed));

                // call the accept signal but only once for this transaction
                if(!trx->accepted) {
                    emit(self.accepted_transaction, trx);
                    trx->accepted = true;
                }

                emit(self.applied_transaction, trace);

                restore.cancel();

                if(!implicit) {
                    unapplied_transactions.erase(trx->signed_id);
                }
                return trace;
            }
            catch(const fc::exception& e) {
                trace->except     = e;
                trace->except_ptr = std::current_exception();
            }
            if(!failure_is_subjective(*trace->except)) {
                unapplied_transactions.erase(trx->signed_id);
            }

            return trace;
        }
        FC_CAPTURE_AND_RETHROW((trace))
    }  /// push_transaction

    void
    start_block(block_timestamp_type when, uint16_t confirm_block_count) {
        FC_ASSERT(!pending);

        FC_ASSERT(db.revision() == head->block_num, "",
                  ("db.revision()", db.revision())("controller_head_block", head->block_num)("fork_db_head_block", fork_db.head()->block_num));

        auto guard_pending = fc::make_scoped_exit([this]() {
            pending.reset();
        });

        pending = pending_state(db.start_undo_session(true), token_db.new_savepoint_session(db.revision()));

        pending->_pending_block_state                   = std::make_shared<block_state>(*head, when);  // promotes pending schedule (if any) to active
        pending->_pending_block_state->in_current_chain = true;

        pending->_pending_block_state->set_confirmed(confirm_block_count);

        auto was_pending_promoted = pending->_pending_block_state->maybe_promote_pending();

        const auto& gpo = db.get<global_property_object>();
        if(gpo.proposed_schedule_block_num.valid() &&                                                          // if there is a proposed schedule that was proposed in a block ...
           (*gpo.proposed_schedule_block_num <= pending->_pending_block_state->dpos_irreversible_blocknum) &&  // ... that has now become irreversible ...
           pending->_pending_block_state->pending_schedule.producers.size() == 0 &&                            // ... and there is room for a new pending schedule ...
           !was_pending_promoted                                                                               // ... and not just because it was promoted to active at the start of this block, then:
        ) {
            // Promote proposed schedule to pending schedule.
            ilog("promoting proposed schedule (set in block ${proposed_num}) to pending; current block: ${n} lib: ${lib} schedule: ${schedule} ",
                 ("proposed_num", *gpo.proposed_schedule_block_num)("n", pending->_pending_block_state->block_num)
                 ("lib", pending->_pending_block_state->dpos_irreversible_blocknum)("schedule", static_cast<producer_schedule_type>(gpo.proposed_schedule)));
            pending->_pending_block_state->set_new_producers(gpo.proposed_schedule);
            db.modify(gpo, [&](auto& gp) {
                gp.proposed_schedule_block_num = optional<block_num_type>();
                gp.proposed_schedule.clear();
            });
        }

        clear_expired_input_transactions();
        guard_pending.cancel();
    }  // start_block

    void
    sign_block(const std::function<signature_type(const digest_type&)>& signer_callback, bool trust) {
        auto p = pending->_pending_block_state;
        try {
            p->sign(signer_callback, false);
        }
        catch(...) {
            edump((fc::json::to_pretty_string(p->header)));
            throw;
        }

        static_cast<signed_block_header&>(*p->block) = p->header;
    }  /// sign_block

    void
    apply_block(const signed_block_ptr& b, bool trust) {
        try {
            try {
                FC_ASSERT(b->block_extensions.size() == 0, "no supported extensions");
                start_block(b->timestamp, b->confirmed);

                for(const auto& receipt : b->transactions) {
                    auto& pt   = receipt.trx;
                    auto  mtrx = std::make_shared<transaction_metadata>(pt);
                    push_transaction(mtrx, fc::time_point::maximum(), false);
                }

                finalize_block();
                sign_block([&](const auto&) { return b->producer_signature; }, trust);

                // this is implied by the signature passing
                //FC_ASSERT( b->id() == pending->_pending_block_state->block->id(),
                //           "applying block didn't produce expected block id" );

                commit_block(false);
                return;
            }
            catch(const fc::exception& e) {
                edump((e.to_detail_string()));
                abort_block();
                throw;
            }
        }
        FC_CAPTURE_AND_RETHROW()
    }  /// apply_block

    void
    push_block(const signed_block_ptr& b, bool trust) {
        //  idump((fc::json::to_pretty_string(*b)));
        FC_ASSERT(!pending, "it is not valid to push a block when there is a pending block");
        try {
            FC_ASSERT(b);
            auto new_header_state = fork_db.add(b, trust);
            emit(self.accepted_block_header, new_header_state);
            maybe_switch_forks(trust);
        }
        FC_LOG_AND_RETHROW()
    }

    void
    push_confirmation(const header_confirmation& c) {
        FC_ASSERT(!pending, "it is not valid to push a confirmation when there is a pending block");
        fork_db.add(c);
        emit(self.accepted_confirmation, c);
        maybe_switch_forks();
    }

    void
    maybe_switch_forks(bool trust = false) {
        auto new_head = fork_db.head();

        if(new_head->header.previous == head->id) {
            try {
                apply_block(new_head->block, trust);
                fork_db.mark_in_current_chain(new_head, true);
                fork_db.set_validity(new_head, true);
                head = new_head;
            }
            catch(const fc::exception& e) {
                fork_db.set_validity(new_head, false);  // Removes new_head from fork_db index, so no need to mark it as not in the current chain.
                throw;
            }
        }
        else if(new_head->id != head->id) {
            ilog("switching forks from ${current_head_id} (block number ${current_head_num}) to ${new_head_id} (block number ${new_head_num})",
                 ("current_head_id", head->id)("current_head_num", head->block_num)("new_head_id", new_head->id)("new_head_num", new_head->block_num));
            auto branches = fork_db.fetch_branch_from(new_head->id, head->id);

            for(auto itr = branches.second.begin(); itr != branches.second.end(); ++itr) {
                fork_db.mark_in_current_chain(*itr, false);
                pop_block();
            }
            FC_ASSERT(self.head_block_id() == branches.second.back()->header.previous,
                      "loss of sync between fork_db and chainbase during fork switch");  // _should_ never fail

            for(auto ritr = branches.first.rbegin(); ritr != branches.first.rend(); ++ritr) {
                optional<fc::exception> except;
                try {
                    apply_block((*ritr)->block, false /*don't trust*/);
                    head = *ritr;
                    fork_db.mark_in_current_chain(*ritr, true);
                }
                catch(const fc::exception& e) {
                    except = e;
                }
                if(except) {
                    elog("exception thrown while switching forks ${e}", ("e", except->to_detail_string()));

                    while(ritr != branches.first.rend()) {
                        fork_db.set_validity(*ritr, false);
                        ++ritr;
                    }

                    // pop all blocks from the bad fork
                    for(auto itr = (ritr + 1).base(); itr != branches.second.end(); ++itr) {
                        fork_db.mark_in_current_chain(*itr, false);
                        pop_block();
                    }
                    FC_ASSERT(self.head_block_id() == branches.second.back()->header.previous,
                              "loss of sync between fork_db and chainbase during fork switch reversal");  // _should_ never fail

                    // re-apply good blocks
                    for(auto ritr = branches.second.rbegin(); ritr != branches.second.rend(); ++ritr) {
                        apply_block((*ritr)->block, true /* we previously validated these blocks*/);
                        head = *ritr;
                        fork_db.mark_in_current_chain(*ritr, true);
                    }
                    throw *except;
                }  // end if exception
            }      /// end for each block in branch
            ilog("successfully switched fork to new head ${new_head_id}", ("new_head_id", new_head->id));
        }
    }  /// push_block

    void
    abort_block() {
        if(pending) {
            for(const auto& t : pending->_pending_block_state->trxs)
                unapplied_transactions[t->signed_id] = t;
            pending.reset();
        }
    }

    bool
    should_enforce_runtime_limits() const {
        return false;
    }

    void
    set_action_merkle() {
        vector<digest_type> action_digests;
        action_digests.reserve(pending->_actions.size());
        for(const auto& a : pending->_actions)
            action_digests.emplace_back(a.digest());

        pending->_pending_block_state->header.action_mroot = merkle(move(action_digests));
    }

    void
    set_trx_merkle() {
        vector<digest_type> trx_digests;
        const auto&         trxs = pending->_pending_block_state->block->transactions;
        trx_digests.reserve(trxs.size());
        for(const auto& a : trxs)
            trx_digests.emplace_back(a.digest());

        pending->_pending_block_state->header.transaction_mroot = merkle(move(trx_digests));
    }

    void
    finalize_block() {
        FC_ASSERT(pending, "it is not valid to finalize when there is no pending block");
        try {
            /*
      ilog( "finalize block ${n} (${id}) at ${t} by ${p} (${signing_key}); schedule_version: ${v} lib: ${lib} #dtrxs: ${ndtrxs} ${np}",
            ("n",pending->_pending_block_state->block_num)
            ("id",pending->_pending_block_state->header.id())
            ("t",pending->_pending_block_state->header.timestamp)
            ("p",pending->_pending_block_state->header.producer)
            ("signing_key", pending->_pending_block_state->block_signing_key)
            ("v",pending->_pending_block_state->header.schedule_version)
            ("lib",pending->_pending_block_state->dpos_irreversible_blocknum)
            ("ndtrxs",db.get_index<generated_transaction_multi_index,by_trx_id>().size())
            ("np",pending->_pending_block_state->header.new_producers)
            );
      */
            set_action_merkle();
            set_trx_merkle();

            auto p = pending->_pending_block_state;
            p->id  = p->header.id();

            create_block_summary(p->id);
        }
        FC_CAPTURE_AND_RETHROW()
    }

    void
    create_block_summary(const block_id_type& id) {
        auto block_num = block_header::num_from_id(id);
        auto sid       = block_num & 0xffff;
        db.modify(db.get<block_summary_object, by_id>(sid), [&](block_summary_object& bso) {
            bso.block_id = id;
        });
    }

    void
    clear_expired_input_transactions() {
        //Look for expired transactions in the deduplication list, and remove them.
        auto&       transaction_idx = db.get_mutable_index<transaction_multi_index>();
        const auto& dedupe_index    = transaction_idx.indices().get<by_expiration>();
        auto        now             = self.pending_block_time();
        while((!dedupe_index.empty()) && (now > fc::time_point(dedupe_index.begin()->expiration))) {
            transaction_idx.remove(*dedupe_index.begin());
        }
    }

};  /// controller_impl

controller::controller(const controller::config& cfg)
    : my(new controller_impl(cfg, *this)) {
}

controller::~controller() {
    my->abort_block();
}

void
controller::startup() {
    // ilog( "${c}", ("c",fc::json::to_pretty_string(cfg)) );
    my->add_indices();

    my->head = my->fork_db.head();
    if(!my->head) {
        elog("No head block in fork db, perhaps we need to replay");
        my->init();
    }
    else {
        //  my->db.set_revision( my->head->block_num );
    }
}

chainbase::database&
controller::db() const {
    return my->db;
}

fork_database&
controller::fork_db() const {
    return my->fork_db;
}

token_database&
controller::token_db() const {
    return my->token_db;
}

void
controller::start_block(block_timestamp_type when, uint16_t confirm_block_count) {
    my->start_block(when, confirm_block_count);
}

void
controller::finalize_block() {
    my->finalize_block();
}

void
controller::sign_block(const std::function<signature_type(const digest_type&)>& signer_callback) {
    my->sign_block(signer_callback, false /* don't trust */);
}

void
controller::commit_block() {
    my->commit_block(true);
}

void
controller::abort_block() {
    my->abort_block();
}

void
controller::push_block(const signed_block_ptr& b, bool trust) {
    my->push_block(b, trust);
    log_irreversible_blocks();
}

void
controller::push_confirmation(const header_confirmation& c) {
    my->push_confirmation(c);
}

transaction_trace_ptr
controller::push_transaction(const transaction_metadata_ptr& trx, fc::time_point deadline) {
    return my->push_transaction(trx, deadline, false);
}

uint32_t
controller::head_block_num() const {
    return my->head->block_num;
}
time_point
controller::head_block_time() const {
    return my->head->header.timestamp;
}
block_id_type
controller::head_block_id() const {
    return my->head->id;
}
account_name
controller::head_block_producer() const {
    return my->head->header.producer;
}
const block_header&
controller::head_block_header() const {
    return my->head->header;
}
block_state_ptr
controller::head_block_state() const {
    return my->head;
}

block_state_ptr
controller::pending_block_state() const {
    if(my->pending)
        return my->pending->_pending_block_state;
    return block_state_ptr();
}
time_point
controller::pending_block_time() const {
    FC_ASSERT(my->pending, "no pending block");
    return my->pending->_pending_block_state->header.timestamp;
}

uint32_t
controller::last_irreversible_block_num() const {
    return std::max(my->head->bft_irreversible_blocknum, my->head->dpos_irreversible_blocknum);
}

block_id_type
controller::last_irreversible_block_id() const {
    auto        lib_num             = last_irreversible_block_num();
    const auto& tapos_block_summary = db().get<block_summary_object>((uint16_t)lib_num);

    if(block_header::num_from_id(tapos_block_summary.block_id) == lib_num)
        return tapos_block_summary.block_id;

    return fetch_block_by_number(lib_num)->id();
}

const dynamic_global_property_object&
controller::get_dynamic_global_properties() const {
    return my->db.get<dynamic_global_property_object>();
}
const global_property_object&
controller::get_global_properties() const {
    return my->db.get<global_property_object>();
}

/**
 *  This method reads the current dpos_irreverible block number, if it is higher
 *  than the last block number of the log, it grabs the next block from the
 *  fork database, saves it to disk, then removes the block from the fork database.
 *
 *  Any forks built off of a different block with the same number are also pruned.
 */
void
controller::log_irreversible_blocks() {
    /*
   if( !my->blog.head() )
      my->blog.read_head();

   const auto& log_head = my->blog.head();
   auto lib = my->head->dpos_irreversible_blocknum;


   if( lib > 2 ) {
      if( log_head && log_head->block_num() > lib ) {
         auto blk = my->fork_db.get_block_in_current_chain_by_num( lib - 1 );
         FC_ASSERT( blk, "unable to find block state", ("block_num",lib-1));
         my->fork_db.prune( blk  );
         my->db.commit( lib -1 );
         return;
      }

      while( log_head && (log_head->block_num()+1) < lib ) {
         auto lhead = log_head->block_num();
         auto blk = my->fork_db.get_block_in_current_chain_by_num( lhead + 1 );
         FC_ASSERT( blk, "unable to find block state", ("block_num",lhead+1));
         irreversible_block( blk );

         if( !my->replaying ) {
            my->blog.append( blk->block );
         }

         my->fork_db.prune( blk );
         my->db.commit( lhead );
      }
   }
   */
}
signed_block_ptr
controller::fetch_block_by_id(block_id_type id) const {
    auto state = my->fork_db.get_block(id);
    if(state)
        return state->block;
    auto bptr = fetch_block_by_number(block_header::num_from_id(id));
    if(bptr && bptr->id() == id)
        return bptr;
    return signed_block_ptr();
}

signed_block_ptr
controller::fetch_block_by_number(uint32_t block_num) const {
    try {
        auto blk_state = my->fork_db.get_block_in_current_chain_by_num(block_num);
        if(blk_state) {
            return blk_state->block;
        }

        return my->blog.read_block_by_num(block_num);
    }
    FC_CAPTURE_AND_RETHROW((block_num))
}

block_id_type
controller::get_block_id_for_num(uint32_t block_num) const {
    try {
        auto blk_state = my->fork_db.get_block_in_current_chain_by_num(block_num);
        if(blk_state) {
            return blk_state->id;
        }

        auto signed_blk = my->blog.read_block_by_num(block_num);

        EVT_ASSERT(BOOST_LIKELY(signed_blk != nullptr), unknown_block_exception,
                   "Could not find block: ${block}", ("block", block_num));

        return signed_blk->id();
    }
    FC_CAPTURE_AND_RETHROW((block_num))
}

void
controller::pop_block() {
    my->pop_block();
}

bool
controller::set_proposed_producers(vector<producer_key> producers) {
    const auto& gpo           = get_global_properties();
    auto        cur_block_num = head_block_num() + 1;

    if(gpo.proposed_schedule_block_num.valid()) {
        if(*gpo.proposed_schedule_block_num != cur_block_num)
            return false;  // there is already a proposed schedule set in a previous block, wait for it to become pending

        if(std::equal(producers.begin(), producers.end(),
                      gpo.proposed_schedule.producers.begin(), gpo.proposed_schedule.producers.end()))
            return false;  // the proposed producer schedule does not change
    }

    producer_schedule_type sch;

    decltype(sch.producers.cend()) end;
    decltype(end)                  begin;

    if(my->pending->_pending_block_state->pending_schedule.producers.size() == 0) {
        const auto& active_sch = my->pending->_pending_block_state->active_schedule;
        begin                  = active_sch.producers.begin();
        end                    = active_sch.producers.end();
        sch.version            = active_sch.version + 1;
    }
    else {
        const auto& pending_sch = my->pending->_pending_block_state->pending_schedule;
        begin                   = pending_sch.producers.begin();
        end                     = pending_sch.producers.end();
        sch.version             = pending_sch.version + 1;
    }

    if(std::equal(producers.begin(), producers.end(), begin, end))
        return false;  // the producer schedule would not change

    sch.producers = std::move(producers);

    my->db.modify(gpo, [&](auto& gp) {
        gp.proposed_schedule_block_num = cur_block_num;
        gp.proposed_schedule           = std::move(sch);
    });
    return true;
}

const producer_schedule_type&
controller::active_producers() const {
    if(!(my->pending))
        return my->head->active_schedule;
    return my->pending->_pending_block_state->active_schedule;
}

const producer_schedule_type&
controller::pending_producers() const {
    if(!(my->pending))
        return my->head->pending_schedule;
    return my->pending->_pending_block_state->pending_schedule;
}

optional<producer_schedule_type>
controller::proposed_producers() const {
    const auto& gpo = get_global_properties();
    if(!gpo.proposed_schedule_block_num.valid())
        return optional<producer_schedule_type>();

    return gpo.proposed_schedule;
}

const apply_handler*
controller::find_apply_handler(action_name act) const {
    auto handler = my->apply_handlers.find(act);
    if(handler != my->apply_handlers.end()) {
        return &handler->second;
    }
    return nullptr;
}

const abi_serializer&
controller::get_abi_serializer() const {
    return my->system_api;
}

vector<transaction_metadata_ptr>
controller::get_unapplied_transactions() const {
    vector<transaction_metadata_ptr> result;
    result.reserve(my->unapplied_transactions.size());
    for(const auto& entry : my->unapplied_transactions) {
        result.emplace_back(entry.second);
    }
    return result;
}

void
controller::drop_unapplied_transaction(const transaction_metadata_ptr& trx) {
    my->unapplied_transactions.erase(trx->signed_id);
}

void
controller::validate_expiration(const transaction& trx) const {
    try {
        const auto& chain_configuration = get_global_properties().configuration;

        EVT_ASSERT(time_point(trx.expiration) >= pending_block_time(),
                   expired_tx_exception,
                   "transaction has expired, "
                   "expiration is ${trx.expiration} and pending block time is ${pending_block_time}",
                   ("trx.expiration", trx.expiration)("pending_block_time", pending_block_time()));
        EVT_ASSERT(time_point(trx.expiration) <= pending_block_time() + fc::seconds(chain_configuration.max_transaction_lifetime),
                   tx_exp_too_far_exception,
                   "Transaction expiration is too far in the future relative to the reference time of ${reference_time}, "
                   "expiration is ${trx.expiration} and the maximum transaction lifetime is ${max_til_exp} seconds",
                   ("trx.expiration", trx.expiration)("reference_time", pending_block_time())("max_til_exp", chain_configuration.max_transaction_lifetime));
    }
    FC_CAPTURE_AND_RETHROW((trx))
}

void
controller::validate_tapos(const transaction& trx) const {
    try {
        const auto& tapos_block_summary = db().get<block_summary_object>((uint16_t)trx.ref_block_num);

        //Verify TaPoS block summary has correct ID prefix, and that this block's time is not past the expiration
        EVT_ASSERT(trx.verify_reference_block(tapos_block_summary.block_id), invalid_ref_block_exception,
                   "Transaction's reference block did not match. Is this transaction from a different fork?",
                   ("tapos_summary", tapos_block_summary));
    }
    FC_CAPTURE_AND_RETHROW()
}

flat_set<public_key_type>
controller::get_required_keys(const transaction& trx, const flat_set<public_key_type>& candidate_keys) const {
    using namespace __internal;
    auto checker = get_auth_checker(my->token_db, candidate_keys);

    for(const auto& act : trx.actions) {
        EVT_ASSERT(checker.satisfied(act), tx_missing_sigs,
                   "${name} action in domain: ${domain} with key: ${key} authorized failed",
                   ("domain", act.domain)("key", act.key)("name", act.name));
    }

    return checker.used_keys();
}

}}  // namespace evt::chain