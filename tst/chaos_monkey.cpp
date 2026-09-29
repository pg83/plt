#include "chaos_monkey.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

using namespace plt;

namespace {
    // PLT_CHAOS=name@skip,... : pass skip matching calls, fail the next
    // one once. Each process starts with a fresh, reproducible script.
    const char* const names[] = {
        "keymap-map",
        "keymap-compile",
        "keymap-state",
        "compose-table",
        "selection-pipe",
        "selection-flags",
        "selection-read",
        "selection-write",
        "read-interrupted",
        "write-interrupted",
        "display-link",
        "display-callback",
        "poll-interrupted",
    };
    static_assert(sizeof(names) / sizeof(*names) == (unsigned)Fault::Count);

    struct Script {
        Script();
        long remaining[(unsigned)Fault::Count];
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    };
}

Script::Script() {
    for (long& value : remaining) {
        value = -1;
    }
    const char* env = getenv("PLT_CHAOS");
    if (env == nullptr || *env == 0) {
        return;
    }
    char* words = strdup(env);
    char* state = nullptr;
    for (char* word = strtok_r(words, ",", &state); word != nullptr; word = strtok_r(nullptr, ",", &state)) {
        char* at = strchr(word, '@');
        if (at == nullptr) {
            fprintf(stderr, "invalid PLT_CHAOS rule: %s\n", word);
            abort();
        }
        *at++ = 0;
        char* end = nullptr;
        long skip = strtol(at, &end, 10);
        bool found = false;
        for (unsigned i = 0; i != (unsigned)Fault::Count; ++i) {
            if (strcmp(names[i], word) == 0 && *at != 0 && *end == 0 && skip >= 0 && remaining[i] == -1) {
                remaining[i] = skip;
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "invalid PLT_CHAOS rule: %s@%s\n", word, at);
            abort();
        }
    }
    free(words);
}

bool plt::chaos(Fault fault) {
    static Script script;
    pthread_mutex_lock(&script.mutex);
    long& remaining = script.remaining[(unsigned)fault];
    const bool fail = remaining == 0;
    if (remaining >= 0) {
        --remaining;
    }
    pthread_mutex_unlock(&script.mutex);
    if (fail) {
        fprintf(stderr, "CHAOS %s\n", names[(unsigned)fault]);
        errno = fault == Fault::ReadInterrupted || fault == Fault::WriteInterrupted || fault == Fault::PollInterrupted ? EINTR : EIO;
    }
    return fail;
}
