static unsigned char g_last_cc_value[MIDI_CHANNEL_COUNT][128];
static unsigned char g_last_cc_seen[MIDI_CHANNEL_COUNT][128];
static int g_last_bend_value[MIDI_CHANNEL_COUNT];
static unsigned char g_last_bend_seen[MIDI_CHANNEL_COUNT];

static void vor_reset_redundancy_shadow(void) {
    memset(g_last_cc_value, 0, sizeof(g_last_cc_value));
    memset(g_last_cc_seen, 0, sizeof(g_last_cc_seen));
    memset(g_last_bend_value, 0, sizeof(g_last_bend_value));
    memset(g_last_bend_seen, 0, sizeof(g_last_bend_seen));
}

/*
 * Does this event leave the channel exactly as it found it? Only a plain
 * controller or pitch bend repeating its current value qualifies. Anything that
 * carries sequence meaning rather than a value -- RPN/NRPN selects, data entry,
 * the all-notes-off family -- always breaks, because a repeat of those is a
 * second real action, not a no-op.
 */
static int vor_event_is_redundant(int ch, const ch_event *e) {
    if (e->type == EVT_CONTROL_CHANGE) {
        const int cc = e->key & 0x7f;
        switch (cc) {
            case 6: case 38: case 98: case 99: case 100: case 101:
            case 120: case 121: case 122: case 123:
            case 124: case 125: case 126: case 127:
                return 0;
            default:
                break;
        }
        if (!g_last_cc_seen[ch][cc]) {
            g_last_cc_seen[ch][cc] = 1;
            g_last_cc_value[ch][cc] = (unsigned char)(e->value & 0x7f);
            return 0;
        }
        if (g_last_cc_value[ch][cc] == (unsigned char)(e->value & 0x7f))
            return 1;
        g_last_cc_value[ch][cc] = (unsigned char)(e->value & 0x7f);
        return 0;
    }
    if (e->type == EVT_PITCH_BEND) {
        if (!g_last_bend_seen[ch]) {
            g_last_bend_seen[ch] = 1;
            g_last_bend_value[ch] = e->value;
            return 0;
        }
        if (g_last_bend_value[ch] == e->value)
            return 1;
        g_last_bend_value[ch] = e->value;
        return 0;
    }
    return 0;
}

