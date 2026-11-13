//
// QCAHTC.cpp - HTC control and endpoint transport for QCA9377.
//
#include "QCAHTC.hpp"
#include "QCAPCIDevice.hpp"
#include "QCACopyEngine.hpp"

static const u16 HTC_MSG_READY = 1;
static const u16 HTC_MSG_CONNECT_SERVICE = 2;
static const u16 HTC_MSG_CONNECT_SERVICE_RESP = 3;
static const u16 HTC_MSG_SETUP_COMPLETE_EX = 5;
static const u16 HTC_CONN_DISABLE_CREDIT_FLOW = 1U << 3;
static const u16 HTC_CONN_RECV_ALLOC_1_4 = 0U << 8;

struct __attribute__((packed)) htc_control_header { __le16 messageId; };
struct __attribute__((packed)) htc_ready_message {
    __le16 credits;
    __le16 creditSize;
    u8 maxEndpoints;
    u8 pad;
};
struct __attribute__((packed)) htc_connect_request {
    __le16 serviceId;
    __le16 flags;
    u8 pad[2];
};
struct __attribute__((packed)) htc_connect_response {
    __le16 serviceId;
    u8 status;
    u8 endpoint;
    __le16 maxMessageSize;
};
struct __attribute__((packed)) htc_setup_complete_extended {
    u8 pad[2];
    __le32 flags;
    u8 maxMessagesPerBundle;
    u8 reserved[3];
};

QCAHTC::QCAHTC(QCAPCIDevice *pci) : fPci(pci), fCreditSize(0)
{
    memset(fCredits, 0, sizeof(fCredits));
    memset(fConnected, 0, sizeof(fConnected));
    memset(fService, 0, sizeof(fService));
    memset(fMaxMessage, 0, sizeof(fMaxMessage));
    memset(fPipeOut, 0, sizeof(fPipeOut));
    memset(fPipeIn, 0, sizeof(fPipeIn));
    memset(fSequence, 0, sizeof(fSequence));
}

QCAHTC::~QCAHTC() {}

bool QCAHTC::init()
{
    QCACopyEngine *ce = fPci->getCopyEngine();
    if (!ce) return false;
    u8 frame[256];
    u32 length = sizeof(frame);
    int retry = 3000;
    while (retry-- > 0 && ce->recv(1, frame, &length) != 0) {
        length = sizeof(frame);
        qca_mdelay(1);
    }
    if (retry <= 0 || length < sizeof(struct ath10k_htc_hdr) + sizeof(htc_control_header) + sizeof(htc_ready_message)) {
        qca_err("Timed out waiting for HTC_READY");
        return false;
    }
    struct ath10k_htc_hdr *transport = (struct ath10k_htc_hdr *)frame;
    struct htc_control_header *control = (struct htc_control_header *)(frame + sizeof(*transport));
    struct htc_ready_message *ready = (struct htc_ready_message *)(control + 1);
    if (transport->eid != ATH10K_HTC_EP_0 || le16_to_cpu(control->messageId) != HTC_MSG_READY) {
        qca_err("Invalid HTC_READY message");
        return false;
    }
    fCreditSize = le16_to_cpu(ready->creditSize);
    if (!fCreditSize || !le16_to_cpu(ready->credits) || ready->maxEndpoints > ATH10K_HTC_EP_COUNT) {
        qca_err("Invalid HTC_READY resources");
        return false;
    }
    fConnected[ATH10K_HTC_EP_0] = true;
    fMaxMessage[ATH10K_HTC_EP_0] = 256;
    fPipeOut[ATH10K_HTC_EP_0] = 0;
    fPipeIn[ATH10K_HTC_EP_0] = 1;
    qca_info("HTC_READY: credits %u, credit size %u, endpoints %u",
             le16_to_cpu(ready->credits), fCreditSize, ready->maxEndpoints);
    return true;
}

bool QCAHTC::connectService(u8 requestedEndpoint, u16 serviceId)
{
    if (requestedEndpoint == ATH10K_HTC_EP_0 || requestedEndpoint >= ATH10K_HTC_EP_COUNT ||
        fConnected[requestedEndpoint]) return false;
    QCACopyEngine *ce = fPci->getCopyEngine();
    if (!ce) return false;

    u8 request[sizeof(struct ath10k_htc_hdr) + sizeof(htc_control_header) + sizeof(htc_connect_request)] = {};
    struct ath10k_htc_hdr *transport = (struct ath10k_htc_hdr *)request;
    struct htc_control_header *control = (struct htc_control_header *)(request + sizeof(*transport));
    struct htc_connect_request *body = (struct htc_connect_request *)(control + 1);
    transport->eid = ATH10K_HTC_EP_0;
    transport->len = cpu_to_le16(sizeof(*control) + sizeof(*body));
    control->messageId = cpu_to_le16(HTC_MSG_CONNECT_SERVICE);
    body->serviceId = cpu_to_le16(serviceId);
    body->flags = cpu_to_le16(HTC_CONN_RECV_ALLOC_1_4 |
        (serviceId == ATH10K_SERVICE_WMI_CONTROL ? 0 : HTC_CONN_DISABLE_CREDIT_FLOW));
    if (ce->send(0, request, sizeof(request)) != 0) return false;

    u8 response[256];
    u32 responseLength = sizeof(response);
    int retry = 3000;
    while (retry-- > 0 && ce->recv(1, response, &responseLength) != 0) {
        responseLength = sizeof(response);
        qca_mdelay(1);
    }
    if (retry <= 0 || responseLength < sizeof(struct ath10k_htc_hdr) + sizeof(htc_control_header) + sizeof(htc_connect_response))
        return false;
    struct ath10k_htc_hdr *rh = (struct ath10k_htc_hdr *)response;
    struct htc_control_header *rc = (struct htc_control_header *)(response + sizeof(*rh));
    struct htc_connect_response *rb = (struct htc_connect_response *)(rc + 1);
    if (rh->eid != ATH10K_HTC_EP_0 || le16_to_cpu(rc->messageId) != HTC_MSG_CONNECT_SERVICE_RESP ||
        le16_to_cpu(rb->serviceId) != serviceId || rb->status != 0 || rb->endpoint != requestedEndpoint ||
        !le16_to_cpu(rb->maxMessageSize)) {
        qca_err("HTC service 0x%04x connect failed (eid %u status %u)", serviceId, rb->endpoint, rb->status);
        return false;
    }
    fConnected[requestedEndpoint] = true;
    fService[requestedEndpoint] = serviceId;
    fMaxMessage[requestedEndpoint] = le16_to_cpu(rb->maxMessageSize);
    fPipeOut[requestedEndpoint] = serviceId == ATH10K_SERVICE_WMI_CONTROL ? 3 : 4;
    fPipeIn[requestedEndpoint] = serviceId == ATH10K_SERVICE_WMI_CONTROL ? 2 : 5;
    fCredits[requestedEndpoint] = 32;
    qca_info("HTC service 0x%04x connected at endpoint %u", serviceId, requestedEndpoint);
    return true;
}

bool QCAHTC::start()
{
    u8 request[sizeof(struct ath10k_htc_hdr) + sizeof(htc_control_header) + sizeof(htc_setup_complete_extended)] = {};
    struct ath10k_htc_hdr *transport = (struct ath10k_htc_hdr *)request;
    struct htc_control_header *control = (struct htc_control_header *)(request + sizeof(*transport));
    transport->eid = ATH10K_HTC_EP_0;
    transport->len = cpu_to_le16(sizeof(*control));
    control->messageId = cpu_to_le16(HTC_MSG_SETUP_COMPLETE_EX);
    transport->len = cpu_to_le16(sizeof(*control) + sizeof(struct htc_setup_complete_extended));
    struct htc_setup_complete_extended *extended = (struct htc_setup_complete_extended *)(control + 1);
    extended->flags = 0;
    extended->maxMessagesPerBundle = 1;
    QCACopyEngine *ce = fPci->getCopyEngine();
    return ce && ce->send(0, request, sizeof(request)) == 0;
}

int QCAHTC::send(u8 epId, void *buf, u32 len)
{
    if (epId >= ATH10K_HTC_EP_COUNT || !fConnected[epId] || !buf ||
        len > fMaxMessage[epId]) return -1;
    u8 frame[4096];
    if (len + sizeof(struct ath10k_htc_hdr) > sizeof(frame)) return -1;
    struct ath10k_htc_hdr *header = (struct ath10k_htc_hdr *)frame;
    memset(header, 0, sizeof(*header));
    header->eid = epId;
    header->len = cpu_to_le16(len);
    header->control1 = fSequence[epId]++;
    memcpy(frame + sizeof(*header), buf, len);
    QCACopyEngine *ce = fPci->getCopyEngine();
    return ce ? ce->send(fPipeOut[epId], frame, len + sizeof(*header)) : -1;
}

int QCAHTC::recv(u8 epId, void *buf, u32 *len)
{
    if (epId >= ATH10K_HTC_EP_COUNT || !fConnected[epId] || !buf || !len) return -1;
    u8 frame[4096];
    u32 frameLength = sizeof(frame);
    QCACopyEngine *ce = fPci->getCopyEngine();
    if (!ce || ce->recv(fPipeIn[epId], frame, &frameLength) != 0 || frameLength < sizeof(struct ath10k_htc_hdr)) return -1;
    struct ath10k_htc_hdr *header = (struct ath10k_htc_hdr *)frame;
    u32 payloadLength = le16_to_cpu(header->len);
    if (header->eid != epId || payloadLength > frameLength - sizeof(*header) || payloadLength > *len) return -1;
    memcpy(buf, frame + sizeof(*header), payloadLength);
    *len = payloadLength;
    return 0;
}
