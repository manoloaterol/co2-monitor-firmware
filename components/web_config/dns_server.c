#include <string.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "web_private.h"

static const char *TAG = "dns";

// Minimal DNS server for the captive portal.
//
// It answers every A query with our own address, whatever the name. That is
// what makes a phone believe the network has no internet and open the setup
// page by itself: iOS asks for captive.apple.com, Android for
// connectivitycheck.gstatic.com, and both must land here.
//
// It is deliberately not a resolver. Anything that is not a question it can
// answer is dropped rather than forwarded, because in provisioning mode there
// is no upstream to forward to.

#define DNS_PORT 53
#define DNS_MAX  512

// Header flags: response, authoritative answer, recursion available.
#define DNS_FLAG_QR 0x8000
#define DNS_FLAG_AA 0x0400
#define DNS_FLAG_RA 0x0080

#define DNS_TYPE_A  1
#define DNS_CLASS_IN 1
#define DNS_TTL      10   // short: the portal disappears as soon as setup ends

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static int s_sock = -1;

// Walks the QNAME, which is a sequence of length-prefixed labels ending in a
// zero byte. Returns the offset just past it, or -1 if it runs off the packet.
static int skip_qname(const uint8_t *buf, int len, int off)
{
    while (off < len) {
        uint8_t l = buf[off];

        if (l == 0) {
            return off + 1;
        }
        // A compression pointer has no place in a question section.
        if ((l & 0xC0) != 0) {
            return -1;
        }
        off += 1 + l;
    }
    return -1;
}

static void dns_task(void *arg)
{
    uint8_t buf[DNS_MAX];
    uint32_t self_ip = (uint32_t)(uintptr_t)arg;

    while (s_sock >= 0) {
        struct sockaddr_storage src;
        socklen_t slen = sizeof(src);

        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (n < (int)sizeof(dns_header_t)) {
            continue;
        }

        dns_header_t *h = (dns_header_t *)buf;
        if (ntohs(h->flags) & DNS_FLAG_QR) {
            continue;   // a response, not a query: answering it would echo forever
        }
        if (ntohs(h->qdcount) != 1) {
            continue;   // no interest in anything but a single plain question
        }

        int qend = skip_qname(buf, n, sizeof(dns_header_t));
        if (qend < 0 || qend + 4 > n) {
            continue;
        }

        uint16_t qtype;
        memcpy(&qtype, buf + qend, sizeof(qtype));
        qend += 4;   // type and class

        h->nscount = 0;
        h->arcount = 0;

        // AAAA, HTTPS and the rest get an empty answer: the name exists, it
        // just has no record of that type. Staying silent made clients wait
        // for their resolver timeout before falling back to the A record.
        if (ntohs(qtype) != DNS_TYPE_A) {
            h->flags   = htons(DNS_FLAG_QR | DNS_FLAG_AA | DNS_FLAG_RA);
            h->ancount = 0;
            sendto(s_sock, buf, qend, 0, (struct sockaddr *)&src, slen);
            continue;
        }

        // The answer repeats the question and appends one record. The name is
        // written as a compression pointer to offset 12, where the question's
        // name already is, which keeps the packet short and is what every
        // resolver expects.
        int len = qend;
        if (len + 16 > (int)sizeof(buf)) {
            continue;
        }

        h->flags   = htons(DNS_FLAG_QR | DNS_FLAG_AA | DNS_FLAG_RA);
        h->ancount = htons(1);

        buf[len++] = 0xC0;
        buf[len++] = 0x0C;

        uint16_t v16 = htons(DNS_TYPE_A);   memcpy(buf + len, &v16, 2); len += 2;
        v16 = htons(DNS_CLASS_IN);          memcpy(buf + len, &v16, 2); len += 2;
        uint32_t v32 = htonl(DNS_TTL);      memcpy(buf + len, &v32, 4); len += 4;
        v16 = htons(4);                     memcpy(buf + len, &v16, 2); len += 2;
        memcpy(buf + len, &self_ip, 4);     len += 4;

        sendto(s_sock, buf, len, 0, (struct sockaddr *)&src, slen);
    }

    vTaskDelete(NULL);
}

esp_err_t dns_server_start(uint32_t self_ip)
{
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        return ESP_FAIL;
    }

    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed");
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    if (xTaskCreate(dns_task, "dns", 3072, (void *)(uintptr_t)self_ip, 4, NULL) != pdPASS) {
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "captive DNS up on port %d", DNS_PORT);
    return ESP_OK;
}

void dns_server_stop(void)
{
    int s = s_sock;
    s_sock = -1;   // the task sees this and exits
    if (s >= 0) {
        close(s);
    }
}
