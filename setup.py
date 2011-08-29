import sys

from distutils.core import setup, Extension

ext = Extension(
    "libspotify_wrapper",
    define_macros = [("MAJOR_VERSION", "0"), ("MINOR_VERSION", "1")],
    include_dirs = [],
    libraries = [],
    library_dirs = [],
    extra_compile_args = [],
    extra_link_args = [],
    language = "c++",
    sources = ["libspotify_wrapper.cpp"])

if sys.platform.startswith("linux"):
    ext.libraries.extend(["SDL", "spotify"])
elif sys.platform == "darwin":
    ext.extra_link_args.extend(["-framework", "libspotify",
                                "-framework", "SDL"])
elif sys.platform == "win32":
    raise RuntimeError("Sorry, you need to compile manually, see README")
else:
    raise RuntimeError("Unknown platform")

setup(name = "libspotify_wrapper",
      version = "0.1",
      description = "Wrapper for libspotify to pipe raw PCM data to stdout.",
      author = "Babar K. Zafar",
      author_email = "babar.zafar@gmail.com",
      url = "https://github.com/bkz",
      long_description = "n/a",
      ext_modules = [ext])
