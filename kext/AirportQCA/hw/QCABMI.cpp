//
//  QCABMI.cpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//

#include "QCABMI.hpp"
#include "QCAPCIDevice.hpp"
#include "QCACopyEngine.hpp"

static const u32 BMI_MAX_MSG = 256;

QCABMI::QCABMI(QCAPCIDevice *pci)
    : fPci(pci), fDoneSent(false)
{
}

QCABMI::~QCABMI()
{
}

int QCABMI::exchangeMessage(const void *req, u32 reqLen, void *resp, u32 *respLen)
{
    if (fDoneSent) {
        qca_warn("BMI message attempted after BMI_DONE");
        return -1;
    }

    QCACopyEngine *ce = fPci->getCopyEngine();
    if (!ce) return -1;

    /* Submit BMI command over CE0 */
    if (ce->send(0, (void*)req, reqLen) != 0) {
        qca_err("Failed to send BMI command via CE0");
        return -1;
    }

    /* If waiting for response, poll CE1 with timeout */
    if (resp && respLen && *respLen > 0) {
        int timeoutMs = 1000;
        while (timeoutMs > 0) {
            if (ce->recv(1, resp, respLen) == 0) {
                return 0; /* Response received successfully */
            }
            qca_mdelay(1);
            timeoutMs--;
        }
        qca_err("Timed out waiting for BMI response on CE1");
        return -1;
    }

    return 0;
}

int QCABMI::getTargetInfo(struct bmi_target_info *info)
{
    if (!info) return -1;

    struct {
        __le32 cmd;
    } req;

    struct {
        __le32 version;
        __le32 type;
    } resp;

    req.cmd = cpu_to_le32(BMI_GET_TARGET_INFO);
    u32 respLen = sizeof(resp);

    int ret = exchangeMessage(&req, sizeof(req), &resp, &respLen);
    if (ret == 0 && respLen >= sizeof(resp)) {
        info->version = le32_to_cpu(resp.version);
        info->type = le32_to_cpu(resp.type);
        qca_info("BMI target info: version = 0x%08x, type = 0x%08x", info->version, info->type);
        return 0;
    }

    return -1;
}

int QCABMI::writeMem(u32 address, const void *data, u32 length)
{
    if (!data || length == 0) return -1;

    /* BMI write command header: cmd, address, length followed by data */
    u8 buffer[BMI_MAX_MSG];
    u32 maxChunk = sizeof(buffer) - 12;
    u32 offset = 0;

    while (offset < length) {
        u32 remaining = length - offset;
        u32 chunk = remaining < maxChunk ? remaining : maxChunk;

        __le32 *hdr = (__le32*)buffer;
        hdr[0] = cpu_to_le32(BMI_WRITE_MEM);
        hdr[1] = cpu_to_le32(address + offset);
        hdr[2] = cpu_to_le32(chunk);
        memcpy(&buffer[12], (const u8*)data + offset, chunk);

        if (exchangeMessage(buffer, chunk + 12, nullptr, nullptr) != 0) {
            qca_err("BMI writeMem failed at offset 0x%08x", offset);
            return -1;
        }

        offset += chunk;
    }

    return 0;
}

int QCABMI::readMem(u32 address, void *data, u32 length)
{
    if (!data || !length) return -1;
    u8 *out = (u8 *)data;
    while (length) {
        u32 chunk = length > 240 ? 240 : length;
        __le32 req[3] = { cpu_to_le32(BMI_READ_MEM), cpu_to_le32(address), cpu_to_le32(chunk) };
        u32 responseLen = chunk;
        if (exchangeMessage(req, sizeof(req), out, &responseLen) != 0 || responseLen != chunk)
            return -1;
        address += chunk;
        out += chunk;
        length -= chunk;
    }
    return 0;
}

int QCABMI::fastDownload(u32 address, const void *data, u32 length)
{
    if (!data || !length) return -1;
    __le32 start[2] = { cpu_to_le32(13), cpu_to_le32(address) }; /* BMI_LZ_STREAM_START */
    if (exchangeMessage(start, sizeof(start), nullptr, nullptr) != 0) return -1;
    const u8 *bytes = (const u8 *)data;
    u32 aligned = length & ~3U;
    u32 offset = 0;
    while (offset < aligned) {
        u32 chunk = aligned - offset;
        if (chunk > BMI_MAX_MSG - 8) chunk = BMI_MAX_MSG - 8;
        chunk &= ~3U;
        u8 msg[BMI_MAX_MSG];
        __le32 *hdr = (__le32 *)msg;
        hdr[0] = cpu_to_le32(14); /* BMI_LZ_DATA */
        hdr[1] = cpu_to_le32(chunk);
        memcpy(msg + 8, bytes + offset, chunk);
        if (exchangeMessage(msg, chunk + 8, nullptr, nullptr) != 0) return -1;
        offset += chunk;
    }
    if (length != aligned) {
        u8 msg[12] = {0};
        __le32 *hdr = (__le32 *)msg;
        hdr[0] = cpu_to_le32(14);
        hdr[1] = cpu_to_le32(4);
        memcpy(msg + 8, bytes + aligned, length - aligned);
        if (exchangeMessage(msg, sizeof(msg), nullptr, nullptr) != 0) return -1;
    }
    __le32 flush[2] = { cpu_to_le32(13), 0 };
    return exchangeMessage(flush, sizeof(flush), nullptr, nullptr);
}

int QCABMI::execute(u32 address, u32 param, u32 *result)
{
    struct {
        __le32 cmd;
        __le32 addr;
        __le32 param;
    } req;

    req.cmd = cpu_to_le32(BMI_EXECUTE);
    req.addr = cpu_to_le32(address);
    req.param = cpu_to_le32(param);

    if (!result) return exchangeMessage(&req, sizeof(req), nullptr, nullptr);
    __le32 response = 0;
    u32 responseLen = sizeof(response);
    if (exchangeMessage(&req, sizeof(req), &response, &responseLen) != 0 || responseLen != sizeof(response))
        return -1;
    *result = le32_to_cpu(response);
    return 0;
}

int QCABMI::setAppStart(u32 address)
{
    struct {
        __le32 cmd;
        __le32 addr;
    } req;

    req.cmd = cpu_to_le32(BMI_SET_APP_START);
    req.addr = cpu_to_le32(address);

    return exchangeMessage(&req, sizeof(req), nullptr, nullptr);
}

int QCABMI::writeSocRegister(u32 address, u32 value)
{
    struct {
        __le32 cmd;
        __le32 addr;
        __le32 val;
    } req;

    req.cmd = cpu_to_le32(BMI_WRITE_SOC_REGISTER);
    req.addr = cpu_to_le32(address);
    req.val = cpu_to_le32(value);

    return exchangeMessage(&req, sizeof(req), nullptr, nullptr);
}

int QCABMI::done()
{
    struct {
        __le32 cmd;
    } req;

    req.cmd = cpu_to_le32(BMI_DONE);
    int ret = exchangeMessage(&req, sizeof(req), nullptr, nullptr);
    if (ret == 0) {
        fDoneSent = true;
        qca_info("BMI_DONE sent; target firmware beginning autonomous execution");
    }
    return ret;
}
