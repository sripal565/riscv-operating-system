#include "syscall.h"
#include "error.h"
#include "string.h"

void main(int argc, char** argv) {
    if (argc != 2) {
        _print("Usage: delete <filename>\n");
        return;
    }
    char *filename = argv[1];
    int rc = _fsdelete(filename);
    if (rc < 0) {
        _print("Error deleting file: ");
    } else {
        _print("File deleted successfully\n");
    }
}