#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

// Answers every A query with self_ip. See dns_server.c for why.
esp_err_t dns_server_start(uint32_t self_ip);
void      dns_server_stop(void);

// Header the page adds to every request that changes something. See
// web_request_allowed() in web_config.c.
#define WEB_GUARD_HEADER "X-CO2-Monitor"

// Answers 403 and returns false for a request that must not be served: one
// addressed to a host name that is not this device's (DNS rebinding) or, when
// it changes state, one without WEB_GUARD_HEADER (cross-site requests).
bool web_request_allowed(httpd_req_t *req, bool changes_state);

// Restarts the device a moment from now, from a task of its own, so the
// response that announces the restart still reaches the browser.
void web_restart_soon(void);

// Receives a firmware image and switches the boot partition. See ota.c.
esp_err_t ota_post(httpd_req_t *req);
