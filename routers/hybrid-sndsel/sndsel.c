#include "m_pd.h"
#include <string.h>
#include <stdint.h>

static t_class *sndsel_class;

#define NUM_SLOTS 14
#define NUM_SOUNDS 4

static const char *slot_names[] = {
    NULL,
    "a1","a2","a3",
    "b1","b2","b3","b4",
    "c1","c2","c3",
    "d1","d2","d3","d4"
};

typedef struct _sndsel {
    t_object x_obj;
    int own_slot;               /* which slot index we're in (1-14), 0 if unknown */
    int sound_tgt[NUM_SOUNDS];
    int sound_led[NUM_SOUNDS];
    int current_sound;
    int current_page;
    int lp_time;
    t_clock *longpress_clock;
    int aux_held;
    uint16_t note_slots[128];
    t_symbol *slot_send[NUM_SLOTS + 1];
    t_symbol *aux_led_sym;
    t_symbol *aux_label_sym;
} t_sndsel;

/* send a note (pitch, velocity) to a slot */
static void send_note(t_sndsel *x, int slot_idx, int pitch, int velocity)
{
    if (slot_idx < 1 || slot_idx > NUM_SLOTS) return;
    t_symbol *dest = x->slot_send[slot_idx];
    if (dest->s_thing) {
        t_atom args[2];
        SETFLOAT(&args[0], (t_float)pitch);
        SETFLOAT(&args[1], (t_float)velocity);
        pd_list(dest->s_thing, &s_list, 2, args);
    }
}

/* update aux LED and label */
static void sndsel_update_aux(t_sndsel *x)
{
    int snd = x->current_sound;
    int tgt = x->sound_tgt[snd];

    /* LED color */
    if (x->aux_led_sym->s_thing) {
        t_atom a;
        SETFLOAT(&a, (t_float)x->sound_led[snd]);
        pd_list(x->aux_led_sym->s_thing, &s_list, 1, &a);
    }

    /* label: "N:slotname" */
    if (x->aux_label_sym->s_thing) {
        char buf[16];
        const char *sname = (tgt >= 1 && tgt <= NUM_SLOTS) ? slot_names[tgt] : "??";
        snprintf(buf, sizeof(buf), "%d:%s", snd + 1, sname);
        t_atom a;
        SETSYMBOL(&a, gensym(buf));
        pd_typedmess(x->aux_label_sym->s_thing, &s_symbol, 1, &a);
    }
}

/* note input: list (pitch velocity) */
static void sndsel_list(t_sndsel *x, t_symbol *s, int argc, t_atom *argv)
{
    (void)s;
    if (argc < 2) return;
    int pitch = (int)atom_getfloat(&argv[0]);
    int velocity = (int)atom_getfloat(&argv[1]);
    if (pitch < 0 || pitch > 127) return;

    if (velocity > 0) {
        /* note-on */
        int target = x->sound_tgt[x->current_sound];
        if (target < 1 || target > NUM_SLOTS || target == x->own_slot) return;

        /* if this note is already held somewhere, send note-off first */
        if (x->note_slots[pitch]) {
            uint16_t mask = x->note_slots[pitch];
            for (int s = 1; s <= NUM_SLOTS; s++) {
                if (mask & (1 << (s - 1)))
                    send_note(x, s, pitch, 0);
            }
        }

        x->note_slots[pitch] = (uint16_t)(1 << (target - 1));
        send_note(x, target, pitch, velocity);
    } else {
        /* note-off: send to whatever slot received the note-on */
        uint16_t mask = x->note_slots[pitch];
        if (mask) {
            for (int s = 1; s <= NUM_SLOTS; s++) {
                if (mask & (1 << (s - 1)))
                    send_note(x, s, pitch, 0);
            }
            x->note_slots[pitch] = 0;
        }
    }
}

/* long press clock callback */
static void sndsel_longpress_tick(t_sndsel *x)
{
    x->aux_held = 0;
    x->current_page ^= 1;
    x->current_sound = x->current_page * 2;
    sndsel_update_aux(x);
}

/* aux button input */
static void sndsel_aux(t_sndsel *x, t_floatarg f)
{
    int val = (int)f;
    if (val == 1) {
        /* press: toggle within page immediately */
        x->aux_held = 1;
        x->current_sound ^= 1;
        sndsel_update_aux(x);
        clock_delay(x->longpress_clock, (double)x->lp_time);
    } else {
        /* release */
        if (x->aux_held) {
            clock_unset(x->longpress_clock);
            x->aux_held = 0;
        }
    }
}

/* parameter setters */
static void sndsel_s1_tgt(t_sndsel *x, t_floatarg f) { x->sound_tgt[0] = (int)f; sndsel_update_aux(x); }
static void sndsel_s2_tgt(t_sndsel *x, t_floatarg f) { x->sound_tgt[1] = (int)f; sndsel_update_aux(x); }
static void sndsel_s3_tgt(t_sndsel *x, t_floatarg f) { x->sound_tgt[2] = (int)f; sndsel_update_aux(x); }
static void sndsel_s4_tgt(t_sndsel *x, t_floatarg f) { x->sound_tgt[3] = (int)f; sndsel_update_aux(x); }

static void sndsel_s1_led(t_sndsel *x, t_floatarg f) { x->sound_led[0] = (int)f; sndsel_update_aux(x); }
static void sndsel_s2_led(t_sndsel *x, t_floatarg f) { x->sound_led[1] = (int)f; sndsel_update_aux(x); }
static void sndsel_s3_led(t_sndsel *x, t_floatarg f) { x->sound_led[2] = (int)f; sndsel_update_aux(x); }
static void sndsel_s4_led(t_sndsel *x, t_floatarg f) { x->sound_led[3] = (int)f; sndsel_update_aux(x); }


/* constructor */
static void *sndsel_new(t_symbol *id)
{
    t_sndsel *x = (t_sndsel *)pd_new(sndsel_class);

    /* detect own slot index */
    x->own_slot = 0;
    for (int i = 1; i <= NUM_SLOTS; i++) {
        if (strcmp(id->s_name, slot_names[i]) == 0) {
            x->own_slot = i;
            break;
        }
    }

    /* defaults */
    x->sound_tgt[0] = 2;
    x->sound_tgt[1] = 4;
    x->sound_tgt[2] = 8;
    x->sound_tgt[3] = 11;
    x->sound_led[0] = 2;
    x->sound_led[1] = 1;
    x->sound_led[2] = 4;
    x->sound_led[3] = 3;
    x->current_sound = 0;
    x->current_page = 0;
    x->lp_time = 500;
    x->aux_held = 0;
    memset(x->note_slots, 0, sizeof(x->note_slots));

    /* cache slot send symbols */
    char buf[32];
    for (int i = 1; i <= NUM_SLOTS; i++) {
        snprintf(buf, sizeof(buf), "notesIn-%s", slot_names[i]);
        x->slot_send[i] = gensym(buf);
    }

    /* cache aux symbols */
    snprintf(buf, sizeof(buf), "aux-led-%s", id->s_name);
    x->aux_led_sym = gensym(buf);
    snprintf(buf, sizeof(buf), "aux-label-%s", id->s_name);
    x->aux_label_sym = gensym(buf);

    /* create long press clock */
    x->longpress_clock = clock_new(x, (t_method)sndsel_longpress_tick);

    return x;
}

/* destructor */
static void sndsel_free(t_sndsel *x)
{
    clock_free(x->longpress_clock);
}

void sndsel_setup(void)
{
    sndsel_class = class_new(gensym("sndsel"),
        (t_newmethod)sndsel_new,
        (t_method)sndsel_free,
        sizeof(t_sndsel),
        0,
        A_SYMBOL, 0);

    class_addlist(sndsel_class, sndsel_list);

    class_addmethod(sndsel_class, (t_method)sndsel_aux,     gensym("aux"),     A_FLOAT, 0);
    class_addmethod(sndsel_class, (t_method)sndsel_s1_tgt,  gensym("s1_tgt"),  A_FLOAT, 0);
    class_addmethod(sndsel_class, (t_method)sndsel_s2_tgt,  gensym("s2_tgt"),  A_FLOAT, 0);
    class_addmethod(sndsel_class, (t_method)sndsel_s3_tgt,  gensym("s3_tgt"),  A_FLOAT, 0);
    class_addmethod(sndsel_class, (t_method)sndsel_s4_tgt,  gensym("s4_tgt"),  A_FLOAT, 0);
    class_addmethod(sndsel_class, (t_method)sndsel_s1_led,  gensym("s1_led"),  A_FLOAT, 0);
    class_addmethod(sndsel_class, (t_method)sndsel_s2_led,  gensym("s2_led"),  A_FLOAT, 0);
    class_addmethod(sndsel_class, (t_method)sndsel_s3_led,  gensym("s3_led"),  A_FLOAT, 0);
    class_addmethod(sndsel_class, (t_method)sndsel_s4_led,  gensym("s4_led"),  A_FLOAT, 0);
}
