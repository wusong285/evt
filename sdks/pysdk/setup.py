from setuptools import setup, find_packages

setup(
    name="evt-pysdk",
    version="0.1",
    author="everiToken",
    author_email="help@everitoken.io",
    description="Python SDK library for everiToken",
    long_description=open("README.rst").read(),
    license="MIT",
    url="https://github.com/everitoken/evt/tree/master/sdks/pysdk",
    packages=find_packages(),
    install_requires=['pyevt'],
    classifiers=[
        "Programming Language :: Python :: 3",
        "License :: OSI Approved :: MIT License",
        "Operating System :: POSIX :: Linux",
        "Operating System :: MacOS :: MacOS X"
    ],
)