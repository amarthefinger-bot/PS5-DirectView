// Placeholder so chiaki-ng's src/aia.h compiles without libcurl.
// aia.c (PSN certificate fetching for internet Remote Play) is not built;
// see ../../chiaki_lan_stubs.c. Only the opaque handle type is referenced.
#ifndef DIRECTVIEW_CURL_SHIM_H
#define DIRECTVIEW_CURL_SHIM_H
typedef void CURL;
#endif
