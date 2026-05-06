Source code orgniazation:
- ui logic must be in main.c
- core logic must be in core.c
- specific logics for different platforms must be in linux.c, windows.c, and darwin.m (macOS)
- must not create source files anymore.

Coding rules:
- must follow C-coding style
- always search (like context7, search,...) info about libraries, C API on platforms,... before perform
- always use English in codebase
- must not modify vendors/*