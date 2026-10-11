#ifndef LGNOME_TEST_LS2_H
#define LGNOME_TEST_LS2_H
#include <stdbool.h>
#include <glib.h>
typedef unsigned LSMessageToken;
typedef struct LSHandle { int unused; } LSHandle;
typedef struct LSMessage { LSMessageToken token; const char *payload; } LSMessage;
typedef struct LSError { const char *message; } LSError;
typedef bool (*LSFilterFunc)(LSHandle *, LSMessage *, void *);
void LSErrorInit(LSError *error);
void LSErrorFree(LSError *error);
bool LSCallCancel(LSHandle *handle, LSMessageToken token, LSError *error);
bool LSCallOneReply(LSHandle *handle, const char *uri, const char *payload, LSFilterFunc callback,
                    void *ctx, LSMessageToken *token, LSError *error);
LSMessageToken LSMessageGetResponseToken(LSMessage *message);
const char *LSMessageGetPayload(LSMessage *message);
bool LSRegister(const char *name, LSHandle **handle, LSError *error);
bool LSGmainAttach(LSHandle *handle, GMainLoop *loop, LSError *error);
bool LSUnregister(LSHandle *handle, LSError *error);
#endif
