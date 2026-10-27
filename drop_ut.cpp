#include "drop.h"

#include <std/tst/ut.h>
#include <std/lib/buffer.h>

using namespace plt;
using namespace stl;

STD_TEST_SUITE(DropUtilities) {
    STD_TEST(UriListIteration) {
        StringView payload(u8"file:///a\r\n# comment\r\n\r\nhttps://x/y\nlast");
        StringView entry;
        STD_INSIST(nextUriListEntry(payload, entry));
        STD_INSIST(entry == StringView(u8"file:///a"));
        STD_INSIST(nextUriListEntry(payload, entry));
        STD_INSIST(entry == StringView(u8"https://x/y"));
        STD_INSIST(nextUriListEntry(payload, entry));
        STD_INSIST(entry == StringView(u8"last"));
        STD_INSIST(!nextUriListEntry(payload, entry));
        STD_INSIST(payload.empty());
    }

    STD_TEST(FileUriDecoding) {
        Buffer path;
        STD_INSIST(fileUriToPath(StringView(u8"file:///tmp/a%20b"), path));
        STD_INSIST(StringView(path) == StringView(u8"/tmp/a b"));
        path.reset();
        STD_INSIST(fileUriToPath(StringView(u8"file://localhost/x%C3%A9"), path));
        STD_INSIST(StringView(path) == StringView(u8"/x\xc3\xa9"));
        path.reset();
        STD_INSIST(!fileUriToPath(StringView(u8"https://host/x"), path));
        STD_INSIST(!fileUriToPath(StringView(u8"file://otherhost/x"), path));
        STD_INSIST(!fileUriToPath(StringView(u8"file:///bad%2"), path));
        STD_INSIST(!fileUriToPath(StringView(u8"file:///bad%zz"), path));
        STD_INSIST(!fileUriToPath(StringView(u8"file://noslash"), path));
    }
}
