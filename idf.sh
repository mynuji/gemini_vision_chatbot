#!/bin/bash
export IDF_PATH="$HOME/esp/esp-idf"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.3_py3.9_env"
export PATH="$HOME/.espressif/python_env/idf5.3_py3.9_env/bin:$HOME/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20240530/xtensa-esp-elf/bin:/opt/homebrew/bin:$PATH"

exec "$HOME/.espressif/python_env/idf5.3_py3.9_env/bin/python" "$HOME/esp/esp-idf/tools/idf.py" "$@"
