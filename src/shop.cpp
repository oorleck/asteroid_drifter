// shop.cpp -- the supply depot that opens between levels.
#include "game.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

// ------------------------------------------------------------- the goods --
// Weapons unlock once and stay for the run; the salvo and the nuke are also
// ammunition, so they are the things credits keep flowing into.
bool Game::owned(int item) const {
    switch (item) {
        case ITEM_HOMING: return pl.hasHoming;
        case ITEM_SALVO:  return pl.hasSalvo;       // the launcher; ammo is separate
        case ITEM_FIELD:  return pl.hasField;
        case ITEM_SHIELD: return pl.hasShield;      // fitted; the charge is separate
        case ITEM_FRACTAL: return pl.hasFractal;    // the launcher; the shells are separate
        default:          return false;
    }
}

int Game::priceOf(int item) const {
    switch (item) {
        case ITEM_HOMING: return rules::PRICE_HOMING;
        case ITEM_SALVO:  return pl.hasSalvo ? rules::PRICE_SALVO_AMMO : rules::PRICE_SALVO;
        case ITEM_NUKE:   return rules::PRICE_NUKE;
        case ITEM_FIELD:  return rules::PRICE_FIELD;
        case ITEM_SHIELD: return pl.hasShield ? rules::PRICE_SHIELD_REFILL : rules::PRICE_SHIELD;
        case ITEM_FRACTAL: return pl.hasFractal ? rules::PRICE_FRACTAL_AMMO : rules::PRICE_FRACTAL;
        default:          return 0;
    }
}

bool Game::canBuy(int item) const {
    if (credits < priceOf(item)) return false;
    switch (item) {
        case ITEM_HOMING: return !pl.hasHoming;
        case ITEM_SALVO:  return pl.salvoAmmo < rules::SALVO_MAX;
        case ITEM_NUKE:   return pl.nukeAmmo < rules::NUKE_MAX;
        case ITEM_FIELD:  return !pl.hasField;
        case ITEM_SHIELD: return !pl.hasShield || pl.shield < rules::SHIELD_CAPACITY - 0.5f;
        case ITEM_FRACTAL: return pl.fractalAmmo < rules::FRACTAL_MAX;
        default:          return false;
    }
}

bool Game::buy(int item) {
    auto note = [&](const char* text, bool bad) {
        snprintf(shopNote, sizeof shopNote, "%s", text);
        shopNoteTime = 2.4f;
        shopNoteBad = bad;
        sfxUI(bad ? Sfx::ShopDeny : Sfx::ShopBuy);
    };
    if (item < 0 || item >= ITEM_COUNT) return false;
    if ((item == ITEM_HOMING && pl.hasHoming) || (item == ITEM_FIELD && pl.hasField)) {
        note("ALREADY OWNED", true);
        return false;
    }
    if (item == ITEM_SHIELD && pl.hasShield && pl.shield >= rules::SHIELD_CAPACITY - 0.5f) {
        note("SHIELD IS ALREADY FULL", true);
        return false;
    }
    if ((item == ITEM_SALVO && pl.salvoAmmo >= rules::SALVO_MAX) ||
        (item == ITEM_NUKE  && pl.nukeAmmo  >= rules::NUKE_MAX) ||
        (item == ITEM_FRACTAL && pl.fractalAmmo >= rules::FRACTAL_MAX)) {
        note("MAGAZINE FULL", true);
        return false;
    }
    if (credits < priceOf(item)) {
        note("NOT ENOUGH CREDITS", true);
        return false;
    }

    credits -= priceOf(item);
    switch (item) {
        case ITEM_HOMING:
            pl.hasHoming = true;
            note("HOMING SHELL ACQUIRED   -   PRESS F", false);
            break;
        case ITEM_SALVO:
            pl.hasSalvo = true;
            pl.salvoAmmo = std::min(rules::SALVO_MAX, pl.salvoAmmo + rules::SALVO_LOAD);
            note("MISSILES LOADED   -   PRESS G", false);
            break;
        case ITEM_NUKE:
            pl.nukeAmmo = std::min(rules::NUKE_MAX, pl.nukeAmmo + rules::NUKE_PACK);
            note("NUKES LOADED   -   PRESS N", false);
            break;
        case ITEM_FRACTAL:
            pl.hasFractal = true;
            pl.fractalAmmo = std::min(rules::FRACTAL_MAX, pl.fractalAmmo + rules::FRACTAL_LOAD);
            note("FRACTAL SHELLS LOADED   -   PRESS Z", false);
            break;
        case ITEM_FIELD:
            pl.hasField = true;
            pl.field = 100.0f;
            note("FORCE FIELD INSTALLED   -   PRESS X", false);
            break;
        case ITEM_SHIELD: {
            const bool first = !pl.hasShield;
            pl.hasShield = true;
            pl.shield = rules::SHIELD_CAPACITY;
            note(first ? "BLAST SHIELD FITTED   -   HOLD LEFT ALT" : "SHIELD RECHARGED", false);
            break;
        }
    }
    return true;
}

// ---------------------------------------------------------------- the UI --
// Rows 0..ITEM_COUNT-1 are goods; row ITEM_COUNT is the "continue" button. The
// same layout serves drawing and mouse hit-testing.
void Game::shopRect(int row, float W, float H, float& x, float& y, float& w, float& h) const {
    const float s = clampf(H / 900.0f, 0.7f, 2.0f);
    const float panelW = 700.0f * s;
    const float top = H * 0.5f - 280.0f * s;
    const float rowH = 52.0f * s, gap = 6.0f * s;
    x = W * 0.5f - panelW * 0.5f + 18.0f * s;
    w = panelW - 36.0f * s;
    if (row < ITEM_COUNT) {
        y = top + 104.0f * s + row * (rowH + gap);
        h = rowH;
    } else {
        y = top + 104.0f * s + ITEM_COUNT * (rowH + gap) + 14.0f * s;
        h = 46.0f * s;
    }
}

void Game::updateShop(Renderer& r, const Input& in) {
    const float W = (float)r.fbw, H = (float)r.fbh;
    const int prevHover = shopHover;
    shopHover = -1;
    for (int i = 0; i <= ITEM_COUNT; ++i) {
        float x, y, w, h;
        shopRect(i, W, H, x, y, w, h);
        if (in.mousePx.x >= x && in.mousePx.x <= x + w && in.mousePx.y >= y && in.mousePx.y <= y + h)
            shopHover = i;
    }
    if (shopHover != prevHover && shopHover >= 0) sfxUI(Sfx::ShopHover, 0.8f, shopHover == ITEM_COUNT ? 1.25f : 1.0f);
    if (in.mousePressed[0] && shopHover >= 0) {
        if (shopHover == ITEM_COUNT) { sfxUI(Sfx::ShopContinue); startLevel(level.number + 1); }
        else                         buy(shopHover);
        return;
    }
    for (int i = 0; i < ITEM_COUNT; ++i)
        if (in.pressed['1' + i]) { buy(i); return; }
    if (in.pressed[VK_RETURN] || in.pressed[VK_SPACE]) { sfxUI(Sfx::ShopContinue); startLevel(level.number + 1); }
}

void Game::drawShop(Renderer& r) {
    const float W = (float)r.fbw, H = (float)r.fbh;
    const float s = clampf(H / 900.0f, 0.7f, 2.0f);
    const float panelW = 700.0f * s, panelH = 560.0f * s;
    const float px = W * 0.5f - panelW * 0.5f, py = H * 0.5f - 280.0f * s;
    char buf[128];

    // Panel: a double outline with clipped corners, in the same vector style.
    const float cut = 16.0f * s;
    const v2 outer[8] = {
        v2(px + cut, py), v2(px + panelW - cut, py), v2(px + panelW, py + cut),
        v2(px + panelW, py + panelH - cut), v2(px + panelW - cut, py + panelH),
        v2(px + cut, py + panelH), v2(px, py + panelH - cut), v2(px, py + cut) };
    r.poly(outer, 8, true, pal::HUD, 1.7f);
    const float in2 = 5.0f * s;
    const v2 inner[8] = {
        v2(px + cut + in2, py + in2), v2(px + panelW - cut - in2, py + in2), v2(px + panelW - in2, py + cut + in2),
        v2(px + panelW - in2, py + panelH - cut - in2), v2(px + panelW - cut - in2, py + panelH - in2),
        v2(px + cut + in2, py + panelH - in2), v2(px + in2, py + panelH - cut - in2), v2(px + in2, py + cut + in2) };
    r.poly(inner, 8, true, pal::HUD, 0.6f);

    r.text(v2(px + 26.0f * s, py + 46.0f * s), 28.0f * s, "SUPPLY DEPOT", pal::HUD, 2.1f);
    snprintf(buf, sizeof buf, "LEVEL %d CLEARED   -   NEXT: LEVEL %d", level.number, level.number + 1);
    r.text(v2(px + 28.0f * s, py + 72.0f * s), 10.5f * s, buf, pal::GOAL, 1.5f);
    snprintf(buf, sizeof buf, "CREDITS  %d", credits);
    r.text(v2(px + panelW - 26.0f * s - r.textWidth(20.0f * s, buf), py + 46.0f * s), 20.0f * s, buf, pal::NUKE, 2.2f);
    snprintf(buf, sizeof buf, lives == 1 ? "LAST LIFE" : "LIVES  %d", lives);
    r.text(v2(px + panelW - 26.0f * s - r.textWidth(10.5f * s, buf), py + 72.0f * s), 10.5f * s, buf,
           lives == 1 ? pal::WARN : pal::HUD, 1.5f);

    static const char* names[ITEM_COUNT] = {
        "HOMING SHELL   [F]", "MISSILE SALVO   [G]", "NUKE   [N]", "FORCE FIELD   [X]",
        "BLAST SHIELD   [LEFT ALT]", "FRACTAL SHELL   [Z]" };
    static const char* descs[ITEM_COUNT] = {
        "Charge shot that curves onto the nearest enemy.",
        "Five missiles, each hunting a different target.",
        "Timed grenade with a huge blast. Pack of three.",
        "Bubble that pushes bullets, missiles, drones and rocks away.",
        "A 30 degree plate toward the cursor. Stops fire and blasts.",
        "Splits in two, five times over. Every piece homes." };
    static const Col accents[ITEM_COUNT] = { pal::HOMING, pal::SALVO, pal::NUKE, pal::FIELD, pal::SHIELD, pal::FRACTAL };

    for (int i = 0; i < ITEM_COUNT; ++i) {
        float x, y, w, h;
        shopRect(i, W, H, x, y, w, h);
        const bool hover = shopHover == i;
        const bool can   = canBuy(i);
        const bool have  = (i == ITEM_HOMING && pl.hasHoming) || (i == ITEM_FIELD && pl.hasField) ||
                           (i == ITEM_SHIELD && pl.hasShield && pl.shield >= rules::SHIELD_CAPACITY - 0.5f);
        const Col base = accents[i];
        const float dim = have ? 0.45f : (can ? 1.0f : 0.5f);
        const Col c(base.r * dim, base.g * dim, base.b * dim);
        const float I = (hover ? 2.6f : 1.5f);

        const v2 box[4] = { v2(x, y), v2(x + w, y), v2(x + w, y + h), v2(x, y + h) };
        r.poly(box, 4, true, c, I);
        if (hover) {
            const v2 hb[4] = { v2(x + 3 * s, y + 3 * s), v2(x + w - 3 * s, y + 3 * s),
                               v2(x + w - 3 * s, y + h - 3 * s), v2(x + 3 * s, y + h - 3 * s) };
            r.poly(hb, 4, true, c, 0.9f);
        }

        // Key number in a little square.
        const float kb = 26.0f * s;
        const v2 kbox[4] = { v2(x + 12 * s, y + h * 0.5f - kb * 0.5f), v2(x + 12 * s + kb, y + h * 0.5f - kb * 0.5f),
                             v2(x + 12 * s + kb, y + h * 0.5f + kb * 0.5f), v2(x + 12 * s, y + h * 0.5f + kb * 0.5f) };
        r.poly(kbox, 4, true, c, 1.4f);
        char kn[2] = { (char)('1' + i), 0 };
        r.text(v2(x + 12 * s + kb * 0.5f - r.textWidth(14 * s, kn) * 0.5f, y + h * 0.5f + 7.0f * s), 14.0f * s, kn, c, 1.8f);

        r.text(v2(x + 56.0f * s, y + 25.0f * s), 15.0f * s, names[i], c, I * 0.9f + 0.4f);
        r.text(v2(x + 56.0f * s, y + 47.0f * s), 9.5f * s, descs[i], Col(0.55f, 0.75f, 0.8f) , 1.1f);

        // Right-hand side: what it costs, or that you already have it.
        char right[64], sub[64] = "";
        if (have) {
            snprintf(right, sizeof right, i == ITEM_SHIELD ? "FULL" : "OWNED");
        } else if (i == ITEM_SALVO) {
            snprintf(right, sizeof right, "%d CR", priceOf(i));
            if (pl.hasSalvo) snprintf(sub, sizeof sub, "+%d SALVOS  (HAVE %d/%d)",
                                      rules::SALVO_LOAD, pl.salvoAmmo, rules::SALVO_MAX);
            else             snprintf(sub, sizeof sub, "%d SALVOS INCLUDED", rules::SALVO_LOAD);
        } else if (i == ITEM_FRACTAL) {
            snprintf(right, sizeof right, "%d CR", priceOf(i));
            if (pl.hasFractal) snprintf(sub, sizeof sub, "+%d SHELLS  (HAVE %d/%d)",
                                        rules::FRACTAL_LOAD, pl.fractalAmmo, rules::FRACTAL_MAX);
            else               snprintf(sub, sizeof sub, "%d SHELLS INCLUDED", rules::FRACTAL_LOAD);
        } else if (i == ITEM_SHIELD) {
            snprintf(right, sizeof right, "%d CR", priceOf(i));
            if (pl.hasShield) snprintf(sub, sizeof sub, "CHARGE %d%%", (int)(pl.shield + 0.5f));
            else              snprintf(sub, sizeof sub, "FITTED, FULL");
        } else if (i == ITEM_NUKE) {
            snprintf(right, sizeof right, "%d CR", priceOf(i));
            snprintf(sub, sizeof sub, "HAVE %d/%d", pl.nukeAmmo, rules::NUKE_MAX);
        } else {
            snprintf(right, sizeof right, "%d CR", priceOf(i));
        }
        const float rh = 16.0f * s;
        r.text(v2(x + w - 16.0f * s - r.textWidth(rh, right), y + 25.0f * s), rh, right, c, I * 0.9f + 0.6f);
        if (sub[0]) r.text(v2(x + w - 16.0f * s - r.textWidth(9.0f * s, sub), y + 47.0f * s), 9.0f * s, sub,
                           Col(0.6f, 0.8f, 0.85f), 1.2f);
    }

    // The way out.
    {
        float x, y, w, h;
        shopRect(ITEM_COUNT, W, H, x, y, w, h);
        const bool hover = shopHover == ITEM_COUNT;
        const Col c = pal::GOAL;
        const v2 box[4] = { v2(x, y), v2(x + w, y), v2(x + w, y + h), v2(x, y + h) };
        r.poly(box, 4, true, c, hover ? 2.8f : 1.7f);
        snprintf(buf, sizeof buf, "CONTINUE TO LEVEL %d     [ENTER]", level.number + 1);
        r.text(v2(x + w * 0.5f - r.textWidth(16.0f * s, buf) * 0.5f, y + h * 0.5f + 6.0f * s), 16.0f * s, buf, c,
               hover ? 2.6f : 1.9f);
    }

    if (shopNoteTime > 0.0f && shopNote[0]) {
        const Col c = shopNoteBad ? pal::WARN : pal::GOAL;
        r.text(v2(px + panelW * 0.5f - r.textWidth(12.0f * s, shopNote) * 0.5f, py + panelH - 12.0f * s),
               12.0f * s, shopNote, c, 1.9f * clampf(shopNoteTime * 2.0f, 0.0f, 1.0f));
    }
}
