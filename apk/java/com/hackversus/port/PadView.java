package com.hackversus.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.util.Log;
import android.view.View;

/* On-screen PS3 pad over the game. Each touch is matched to a control; the pressed set goes to native code as
 * SDL GameController buttons on a virtual joystick, which cellPad reads like any other controller. A real
 * gamepad keeps working alongside it. Positions are fractions of the view, so the layout holds on any screen. */
public class PadView extends View {
    /* SDL_GameControllerButton numbering; bits 16/17 stand for the L2/R2 triggers. */
    static final int A = 0, B = 1, X = 2, Y = 3, BACK = 4, START = 6, L1 = 9, R1 = 10,
                     UP = 11, DOWN = 12, LEFT = 13, RIGHT = 14, L2 = 16, R2 = 17;

    static native void nativePad(int buttons);

    private static class Btn {
        final int bit; final float fx, fy, fr; final String label; final boolean round;
        Btn(int bit, float fx, float fy, float fr, String label, boolean round) {
            this.bit = bit; this.fx = fx; this.fy = fy; this.fr = fr; this.label = label; this.round = round;
        }
    }

    /* Face buttons on the right, shoulders on the top corners, SELECT/START in the middle. fr is the touch and
     * drawing radius as a fraction of the view height. */
    private final Btn[] btns = {
        new Btn(Y, 0.875f, 0.50f, 0.085f, "△", true),
        new Btn(X, 0.810f, 0.66f, 0.085f, "□", true),
        new Btn(B, 0.940f, 0.66f, 0.085f, "○", true),
        new Btn(A, 0.875f, 0.82f, 0.085f, "✕", true),
        new Btn(L1, 0.075f, 0.24f, 0.070f, "L1", false),
        new Btn(L2, 0.075f, 0.08f, 0.070f, "L2", false),
        new Btn(R1, 0.925f, 0.24f, 0.070f, "R1", false),
        new Btn(R2, 0.925f, 0.08f, 0.070f, "R2", false),
        new Btn(BACK, 0.43f, 0.92f, 0.055f, "SELECT", false),
        new Btn(START, 0.57f, 0.92f, 0.055f, "START", false),
    };
    /* D-pad: one disc; the touch's direction from its centre picks up to two of the four arrows. */
    private final float padFx = 0.13f, padFy = 0.68f, padFr = 0.20f;

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
                        text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int sent = -1, shown, logged;
    /* The toggle at the top centre hides the controls (for a real gamepad) and shows them again. */
    private boolean hidden;
    private final float togFx = 0.5f, togFy = 0.11f, togFr = 0.035f;

    private boolean onToggle(float x, float y) {
        float w = getWidth(), h = getHeight();
        float dx = x - togFx * w, dy = y - togFy * h, r = togFr * h * 1.6f;
        return dx * dx + dy * dy <= r * r;
    }

    public PadView(Context c) {
        super(c);
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeWidth(3);
        line.setColor(0x99FFFFFF);
        text.setColor(0xCCFFFFFF);
        text.setTextAlign(Paint.Align.CENTER);
    }

    private int dpadBits(float x, float y) {
        float w = getWidth(), h = getHeight();
        float dx = x - padFx * w, dy = y - padFy * h, r = padFr * h;
        if (dx * dx + dy * dy > r * r * 2.25f) return -1;          /* outside, with some slack around the disc */
        if (dx * dx + dy * dy < r * r * 0.04f) return 0;           /* dead centre */
        double a = Math.toDegrees(Math.atan2(-dy, dx));             /* 0 = right, 90 = up */
        if (a < 0) a += 360;
        int bits = 0;
        if (a < 67.5 || a > 292.5)  bits |= 1 << RIGHT;
        if (a > 22.5 && a < 157.5)  bits |= 1 << UP;
        if (a > 112.5 && a < 247.5) bits |= 1 << LEFT;
        if (a > 202.5 && a < 337.5) bits |= 1 << DOWN;
        return bits;
    }

    private int hit(float x, float y) {
        int d = dpadBits(x, y);
        if (d >= 0) return d;
        float w = getWidth(), h = getHeight();
        for (Btn b : btns) {
            float dx = x - b.fx * w, dy = y - b.fy * h, r = b.fr * h * 1.15f;
            if (dx * dx + dy * dy <= r * r) return 1 << b.bit;
        }
        return 0;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int bits = 0, up = -1, act = e.getActionMasked();
        if (act == MotionEvent.ACTION_DOWN || act == MotionEvent.ACTION_POINTER_DOWN) {
            int i = e.getActionIndex();
            if (onToggle(e.getX(i), e.getY(i))) {
                hidden = !hidden;
                if (sent != 0) { sent = 0; shown = 0; nativePad(0); }
                invalidate();
                return true;
            }
        }
        if (hidden) return true;
        if (act == MotionEvent.ACTION_UP || act == MotionEvent.ACTION_POINTER_UP) up = e.getActionIndex();
        if (act != MotionEvent.ACTION_CANCEL)
            for (int i = 0; i < e.getPointerCount(); i++)
                if (i != up) bits |= hit(e.getX(i), e.getY(i));
        if (bits != sent) {
            if (logged++ < 12) Log.i("PadView", "touch " + e.getX() + "," + e.getY() + " bits=" + Integer.toHexString(bits));
            sent = bits;
            nativePad(bits);
            shown = bits;
            invalidate();
        }
        return true;
    }

    @Override
    protected void onDraw(Canvas c) {
        float w = getWidth(), h = getHeight();
        text.setTextSize(h * 0.045f);
        /* The toggle: a small disc, dimmer while the controls are hidden. */
        fill.setColor(hidden ? 0x22FFFFFF : 0x33FFFFFF);
        c.drawCircle(togFx * w, togFy * h, togFr * h, fill);
        c.drawCircle(togFx * w, togFy * h, togFr * h, line);
        text.setColor(hidden ? 0x66FFFFFF : 0xCCFFFFFF);
        float ts0 = text.getTextSize();
        text.setTextSize(ts0 * 0.6f);
        c.drawText(hidden ? "PAD" : "PAD", togFx * w, togFy * h + text.getTextSize() * 0.35f, text);
        text.setTextSize(ts0);
        if (hidden) return;
        /* D-pad */
        float px = padFx * w, py = padFy * h, pr = padFr * h;
        fill.setColor(0x33FFFFFF);
        c.drawCircle(px, py, pr, fill);
        c.drawCircle(px, py, pr, line);
        int[] arrows = { UP, DOWN, LEFT, RIGHT };
        float[][] at = { { 0, -0.6f }, { 0, 0.6f }, { -0.6f, 0 }, { 0.6f, 0 } };
        String[] lab = { "▲", "▼", "◀", "▶" };
        for (int i = 0; i < 4; i++) {
            text.setColor((shown & (1 << arrows[i])) != 0 ? 0xFFFFE066 : 0xCCFFFFFF);
            c.drawText(lab[i], px + at[i][0] * pr, py + at[i][1] * pr + text.getTextSize() * 0.35f, text);
        }
        /* Buttons */
        for (Btn b : btns) {
            boolean on = (shown & (1 << b.bit)) != 0;
            fill.setColor(on ? 0x88FFE066 : 0x33FFFFFF);
            float x = b.fx * w, y = b.fy * h, r = b.fr * h;
            if (b.round) {
                c.drawCircle(x, y, r, fill);
                c.drawCircle(x, y, r, line);
            } else {
                RectF rr = new RectF(x - r * 1.6f, y - r * 0.75f, x + r * 1.6f, y + r * 0.75f);
                c.drawRoundRect(rr, r * 0.5f, r * 0.5f, fill);
                c.drawRoundRect(rr, r * 0.5f, r * 0.5f, line);
            }
            text.setColor(0xCCFFFFFF);
            float ts = text.getTextSize();
            if (!b.round) text.setTextSize(ts * 0.7f);
            c.drawText(b.label, x, y + text.getTextSize() * 0.35f, text);
            text.setTextSize(ts);
        }
    }
}
