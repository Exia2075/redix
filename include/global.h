#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum {
    REDIX_DEFAULT_BACKLOG = 128,
    REDIX_DEFAULT_EVENT_TIMEOUT_MS = 100,
    REDIX_MAX_EVENTS = 1024
};

#ifdef __cplusplus
}
#endif
