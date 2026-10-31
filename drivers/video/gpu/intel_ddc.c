/* Чтение EDID: GMBUS (HDMI/DVI/DP внешние порты) и I2C-over-AUX (eDP ноутбучной панели, DP). Только чтение. */
#define MIGA_TAG "igfx"
#include <miga/log.h>
#include <miga/time.h>
#include <miga/string.h>
#include "intel_priv.h"

#define GMBUS0 0xC5100u
#define GMBUS1 0xC5104u
#define GMBUS2 0xC5108u
#define GMBUS3 0xC510Cu
#define GMBUS4 0xC5110u
#define G1_SW_CLR_INT (1u << 31)
#define G1_SW_RDY     (1u << 30)
#define G1_CYC_WAIT   (1u << 25)
#define G1_CYC_INDEX  (2u << 25)
#define G1_CYC_STOP   (4u << 25)
#define G2_ACTIVE     (1u << 9)
#define G2_NAK        (1u << 10)
#define G2_HW_RDY     (1u << 11)

static int edid_ok(const uint8_t *e)
{
    static const uint8_t m[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    if (memcmp(e, m, 8)) return 0;
    unsigned s = 0;
    for (unsigned i = 0; i < 128; i++) s += e[i];
    return (s & 0xFF) == 0;
}

/* ------------------------------------------------------------------ GMBUS */
static int gm_wait(uint32_t mask, uint32_t *st)
{
    uint64_t t0 = time_now_us();
    while (time_now_us() - t0 < 10000) {
        uint32_t v = igr(GMBUS2);
        if (v == 0xFFFFFFFFu) return -1;
        if (v & G2_NAK) return -1;
        if (v & mask) { if (st) *st = v; return 0; }
    }
    return -1;
}

static void gm_stop(void)
{
    igw(GMBUS1, G1_SW_CLR_INT);
    igw(GMBUS1, 0);
    igw(GMBUS0, 0);
}

static int gm_read(unsigned pin, uint8_t index, uint8_t *buf, unsigned len)
{
    igw(GMBUS0, pin & 0x1F);                                   /* 100 кГц */
    igw(GMBUS4, 0);
    igw(GMBUS1, G1_SW_CLR_INT);
    igw(GMBUS1, G1_SW_RDY | G1_CYC_INDEX | G1_CYC_WAIT | ((uint32_t)index << 8) | (len << 16) | (0x50u << 1) | 1u);
    for (unsigned i = 0; i < len; i += 4) {
        if (gm_wait(G2_HW_RDY, 0)) { gm_stop(); return -1; }
        uint32_t v = igr(GMBUS3);
        for (unsigned k = 0; k < 4 && i + k < len; k++) buf[i + k] = (uint8_t)(v >> (8 * k));
    }
    igw(GMBUS1, G1_CYC_STOP | G1_SW_RDY);
    uint64_t t0 = time_now_us();
    while ((igr(GMBUS2) & G2_ACTIVE) && time_now_us() - t0 < 5000) {}
    gm_stop();
    return 0;
}

static int ddc_gmbus(void)
{
    static const uint8_t pins[] = {5, 4, 6, 2, 3, 1, 9, 10, 11, 12, 13, 14};
    for (unsigned p = 0; p < sizeof pins; p++) {
        if (pins[p] > 7 && IG.ver < 11) continue;
        uint8_t e[256];
        if (gm_read(pins[p], 0, e, 128) || !edid_ok(e)) continue;
        unsigned n = 128;
        if (e[126] && !gm_read(pins[p], 128, e + 128, 128)) n = 256;
        memcpy(IG.edid, e, n);
        IG.edid_len = n;
        pr_info("EDID via GMBUS pin %u (%u bytes)\n", pins[p], n);
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ AUX */
#define AUX_CTL(ch)  (0x64010u + 0x100u * (ch))
#define AUX_DAT(ch, i) (0x64014u + 0x100u * (ch) + 4u * (i))
#define AUX_BUSY (1u << 31)
#define AUX_DONE (1u << 30)
#define AUX_TOUT (1u << 28)
#define AUX_RERR (1u << 25)

static int aux_xfer(unsigned ch, const uint8_t *tx, unsigned txn, uint8_t *rx, unsigned rxmax)
{
    uint32_t ctl = AUX_CTL(ch);
    for (unsigned try = 0; try < 5; try++) {
        uint64_t t0 = time_now_us();
        while ((igr(ctl) & AUX_BUSY) && time_now_us() - t0 < 10000) {}
        if (igr(ctl) & AUX_BUSY) return -1;
        for (unsigned i = 0; i < txn; i += 4) {
            uint32_t v = 0;
            for (unsigned k = 0; k < 4; k++) v = v << 8 | (i + k < txn ? tx[i + k] : 0);
            igw(AUX_DAT(ch, i / 4), v);
        }
        igw(ctl, AUX_BUSY | AUX_DONE | (3u << 26) | AUX_RERR | AUX_TOUT | (txn << 20) | (31u << 5) | 31u);
        t0 = time_now_us();
        uint32_t s;
        while (((s = igr(ctl)) & AUX_BUSY) && time_now_us() - t0 < 20000) {}
        igw(ctl, s | AUX_DONE | AUX_TOUT | AUX_RERR);
        if (s & AUX_BUSY) return -1;
        if (s & (AUX_TOUT | AUX_RERR) || !(s & AUX_DONE)) continue;
        unsigned n = (s >> 20) & 0x1F;
        if (!n) continue;
        if (n > rxmax) n = rxmax;
        for (unsigned i = 0; i < n; i += 4) {
            uint32_t v = igr(AUX_DAT(ch, i / 4));
            for (unsigned k = 0; k < 4 && i + k < n; k++) rx[i + k] = (uint8_t)(v >> (24 - 8 * k));
        }
        return (int)n;
    }
    return -1;
}

/* I2C-over-AUX: запись индекса (MOT), затем чтение кусками по 16 байт. */
static int aux_i2c_block(unsigned ch, uint8_t offset, uint8_t *out)
{
    uint8_t tx[20], rx[20];
    tx[0] = 0x40; tx[1] = 0; tx[2] = 0x50; tx[3] = 0; tx[4] = offset;
    int r = -1;
    for (unsigned t = 0; t < 4; t++) {
        r = aux_xfer(ch, tx, 5, rx, sizeof rx);
        if (r >= 1 && (rx[0] & 0xF0) == 0 && (rx[0] & 0x0C) == 0) break;
        r = -1;
    }
    if (r < 0) return -1;
    for (unsigned i = 0; i < 128; i += 16) {
        tx[0] = (uint8_t)(((i + 16 < 128 ? 5 : 1)) << 4); tx[1] = 0; tx[2] = 0x50; tx[3] = 15;
        for (unsigned t = 0;; t++) {
            r = aux_xfer(ch, tx, 4, rx, sizeof rx);
            if (r == 17 && rx[0] == 0) break;
            if (t >= 4) return -1;
        }
        memcpy(out + i, rx + 1, 16);
    }
    return 0;
}

static int ddc_aux(void)
{
    if (IG.ver < 9 || IG.ver > 13) return -1;
    for (unsigned ch = 0; ch < 4; ch++) {
        uint8_t e[256];
        if (aux_i2c_block(ch, 0, e) || !edid_ok(e)) continue;
        unsigned n = 128;
        if (e[126] && !aux_i2c_block(ch, 128, e + 128)) n = 256;
        memcpy(IG.edid, e, n);
        IG.edid_len = n;
        pr_info("EDID via DP AUX ch %c (%u bytes)\n", 'A' + ch, n);
        return 0;
    }
    return -1;
}

int igfx_ddc_read_edid(void)
{
    IG.edid_len = 0;
    if (!ddc_aux()) return 0;
    return ddc_gmbus();
}
