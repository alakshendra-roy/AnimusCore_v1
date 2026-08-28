from setuptools import setup, find_packages

setup(
    name='animus-core',
    version='1.0.0',
    author='Alakshendra Roy',
    description='High-Performance Low-Latency C++/Python Event Ingestion & SOAR Engine',
    long_description=open('README.md').read(),
    long_description_content_type='text/markdown',
    packages=find_packages(),
    classifiers=[
        'Programming Language :: Python :: 3',
        'Programming Language :: C++',
        'Operating System :: OS Independent',
        'Topic :: Security',
        'Topic :: Software Development :: Libraries',
    ],
    python_requires='>=3.8',
    include_package_data=True,
)
