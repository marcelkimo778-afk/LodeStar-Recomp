#!/usr/bin/env python3
"""swrecomp entry point: adds Engine/Source to sys.path and dispatches to the CLI."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "Source"))   # Engine/Source
from swrecomp.cli import main

if __name__ == "__main__":
    sys.exit(main())
