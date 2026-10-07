"""Extracts the C++ blocks of README.md so the build compiles every one.

Each ```cpp block becomes a file in OUT_DIR:
  - a block with `int main(` is a whole program: program_N.cpp;
  - a block with `TEST(` is a GoogleTest file: test_N.cpp, built and run;
  - any other block is a fragment, compiled inside `main` after a
    declaration named `config`: program_N.cpp.
A block whose first line is `// (needs file inputs)`, or that calls
add_file, is skipped by the environment-only build.

Usage: readme_snippets.py README.md OUT_DIR
"""
import pathlib
import re
import sys

readme, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
for old in out.glob("*.cpp"):
    old.unlink()

blocks = re.findall(r"^```cpp\n(.*?)^```", readme.read_text(), re.S | re.M)
if not blocks:
    sys.exit("no ```cpp blocks in " + str(readme))
for i, code in enumerate(blocks, 1):
    files = "// docuconf-readme: files\n" if "add_file" in code else ""
    origin = f"// README.md, C++ block {i}\n"
    if "TEST(" in code:
        (out / f"test_{i}.cpp").write_text(origin + files + code)
    elif "int main(" in code:
        (out / f"program_{i}.cpp").write_text(origin + files + code)
    else:
        (out / f"program_{i}.cpp").write_text(
            origin + files + "#include <docuconf/docuconf.hpp>\n\n"
            "int main(int argc, char** argv) {\n"
            "    CLI::App app{\"svc\"};\n"
            "    docuconf::Declaration config{app, \"svc\"};\n"
            + code + "\n    DOCUCONF_PARSE(config, argc, argv);\n}\n")
print(f"{len(blocks)} README C++ blocks")
