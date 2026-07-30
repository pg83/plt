#include "test.h"

#include <stdio.h>

namespace plt::test {
    bool mimeClipboard(int fd) {
        static constexpr size_t jxlSize = 128 * 1024;
        static constexpr size_t pngSize = 192 * 1024;
        stl::Buffer jxl = repeated(jxlSize, 'j');
        stl::Buffer png = repeated(pngSize, 'p');
        Client client(fd);
        client.window->requestWriteClipboard(stl::StringView(u8"image/jxl"), stl::StringView(jxl));
        client.window->requestWriteClipboard(stl::StringView(u8"image/png"), stl::StringView(png));
        command(fd, Command::PointerEnter);
        pump(*client.platform);

        const u32 expectedFormats = SourceJxl | SourcePng;
        const Reply formats = command(fd, Command::QuerySourceFormats);
        if (formats.count != expectedFormats) {
            fprintf(stderr, "MIME clipboard: formats=%u, expected %u\n", formats.count, expectedFormats);
            return false;
        }

        if (command(fd, Command::RequestJxlSourceData).count != 1) {
            fprintf(stderr, "MIME clipboard: JXL source was not ready\n");
            return false;
        }
        pump(*client.platform);
        if (command(fd, Command::ReleaseWrite).count != 1) {
            fprintf(stderr, "MIME clipboard: JXL source send did not return\n");
            return false;
        }
        Reply state;
        for (u32 attempt = 0; attempt != 100; ++attempt) {
            pump(*client.platform);
            state = command(fd, Command::QueryWrite);
            if (state.first != 0) {
                break;
            }
        }
        if (state.count != jxlSize || state.first == 0 || state.second != 'j') {
            fprintf(stderr, "MIME clipboard: JXL bytes=%u complete=%d first=%d, expected %zu/1/j\n", state.count, state.first, state.second, jxlSize);
            return false;
        }

        if (command(fd, Command::RequestPngSourceData).count != 1) {
            fprintf(stderr, "MIME clipboard: PNG source was not ready\n");
            return false;
        }
        pump(*client.platform);
        if (command(fd, Command::ReleaseWrite).count != 1) {
            fprintf(stderr, "MIME clipboard: PNG source send did not return\n");
            return false;
        }
        for (u32 attempt = 0; attempt != 100; ++attempt) {
            pump(*client.platform);
            state = command(fd, Command::QueryWrite);
            if (state.first != 0) {
                break;
            }
        }
        if (state.count != pngSize || state.first == 0 || state.second != 'p') {
            fprintf(stderr, "MIME clipboard: PNG bytes=%u complete=%d first=%d, expected %zu/1/p\n", state.count, state.first, state.second, pngSize);
            return false;
        }
        return true;
    }
}
