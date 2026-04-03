from setuptools import setup, Extension

setup(
    ext_modules=[
        Extension(
            "phobic._module",
            sources=["src/phobic/_module.c", "src/phobic/_phobic.c"],
            extra_compile_args=["-O2", "-std=c11", "-Wall", "-Wextra"],
            extra_link_args=[],
        ),
    ],
)
