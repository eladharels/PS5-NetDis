#pragma once

#include "http.h"

// Generated at build time by tools/embed.py.
extern const char netdis_ui_html[];
extern const unsigned char netdis_icon_png[];
extern const unsigned long netdis_icon_png_size;

void netdis_handle(const http_req_t *req, http_resp_t *resp);
