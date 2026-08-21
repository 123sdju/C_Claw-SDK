#include "cc/ports/cc_filesystem.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    static const unsigned char payload[] = {
        0xff, 0xd8, 0x00, 0x7f, 0x80, 0xff, 0xd9
    };
    const char *path = "cclaw_filesystem_binary_test.tmp";
    cc_filesystem_t fs = {0};
    cc_result_t rc = cc_filesystem_get_default(&fs);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return 1;
    }
    cc_result_free(&rc);

    if (!fs.vtable || !fs.vtable->write_bytes) {
        if (fs.vtable && fs.vtable->destroy) fs.vtable->destroy(fs.self);
        return 2;
    }

    rc = fs.vtable->write_bytes(fs.self, path, payload, sizeof(payload));
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        if (fs.vtable->remove) {
            rc = fs.vtable->remove(fs.self, path);
            cc_result_free(&rc);
        }
        fs.vtable->destroy(fs.self);
        return 3;
    }
    cc_result_free(&rc);

    unsigned char actual[sizeof(payload)] = {0};
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (fs.vtable->remove) {
            rc = fs.vtable->remove(fs.self, path);
            cc_result_free(&rc);
        }
        fs.vtable->destroy(fs.self);
        return 4;
    }
    size_t read_size = fread(actual, 1, sizeof(actual), file);
    int extra = fgetc(file);
    int close_rc = fclose(file);

    rc = fs.vtable->remove(fs.self, path);
    int remove_ok = rc.code == CC_OK;
    cc_result_free(&rc);
    fs.vtable->destroy(fs.self);

    if (read_size != sizeof(payload) || extra != EOF || close_rc != 0 ||
        memcmp(actual, payload, sizeof(payload)) != 0 || !remove_ok) {
        return 5;
    }
    return 0;
}
