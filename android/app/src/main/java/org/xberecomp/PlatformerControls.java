package org.xberecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;
import org.libsdl.app.SDLActivity;

/**
 * Touch overlay for a 3D platformer: a floating analog stick on the left and
 * Xbox face/directional buttons on the right. The stick feeds the runtime's
 * virtual controller 0, so the guest sees the same XInput report as a physical
 * pad; the buttons send the matching Xbox button bits.
 *
 * Multi-touch: one pointer owns the stick, each button tracks its own pointer.
 * A second finger on the stick does not steal it from the first.
 */
final class PlatformerControls extends View {
    private static final int GAME = -1;
    private static final int STICK = -2;
    private static final float DEADZONE = 0.16f;

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final ArrayList<Control> buttons = new ArrayList<>();
    private final SparseArray<Integer> owners = new SparseArray<>();
    private int stickPointer = -1;
    private float stickX, stickY;
    private final RectF stickBase = new RectF();
    private final RectF stickKnob = new RectF();
    private float radius;
    private boolean settingsHeld;

    private static final class Control {
        final String label;
        final int button;   // 0 A, 1 B, 2 X, 3 Y, 4 Black, 5 White, 6 Start, 7 Back
        final int dpad;     // -1 for a face button, 0..3 for the D-pad
        final boolean settings;
        final RectF rect;
        int held;
        Control(String label, int button, int dpad, boolean settings, RectF rect) {
            this.label = label; this.button = button; this.dpad = dpad;
            this.settings = settings; this.rect = rect;
        }
    }

    PlatformerControls(Context context) {
        super(context);
        setFocusable(false);
        setClickable(true);
        setContentDescription("Sonic Heroes touch controls");
    }

    private void add(String label, int button, int dpad, float x, float y, float w, float h) {
        buttons.add(new Control(label, button, dpad, false, new RectF(x, y, x + w, y + h)));
    }

    private void addSettings(float x, float y, float w, float h) {
        buttons.add(new Control("Settings", -1, -1, true, new RectF(x, y, x + w, y + h)));
    }

    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        releaseAll();
        buttons.clear();
        final float u = Math.min(w / 13f, h / 6.5f);
        final float gap = u * 0.18f;
        radius = u * 1.05f;
        final float cx = gap + radius;
        final float cy = h - gap - radius;
        stickBase.set(cx - radius, cy - radius, cx + radius, cy + radius);
        stickKnob.set(cx - radius * 0.42f, cy - radius * 0.42f, cx + radius * 0.42f, cy + radius * 0.42f);

        final float bw = u * 1.15f, bh = u * 1.15f;
        final float right = w - gap;
        final float bottom = h - gap;
        // right thumb cluster: jump is the big lower button, action and
        // special sit above it, camera left/right on the outer edge
        add("Jump", 0, -1, right - bw, bottom - bh, bw, bh);                       // A
        add("Action", 1, -1, right - bw * 2 - gap * 0.7f, bottom - bh * 1.05f, bw, bh);  // B
        add("Special", 2, -1, right - bw * 2 - gap * 0.7f, bottom - bh * 2.15f - gap * 0.7f, bw, bh);  // X
        add("CamL", -1, 2, right - bw * 3 - gap * 1.4f, bottom - bh * 0.5f, bw * 0.8f, bh * 0.8f);
        add("CamR", -1, 3, right - bw * 0.8f, bottom - bh * 2.15f - gap * 1.4f, bw * 0.8f, bh * 0.8f);
        add("Start", 6, -1, right - bw, gap, bw, bh * 0.8f);
        addSettings(right - bw * 2 - gap * 0.7f, gap, bw, bh * 0.8f);
    }

    private int hit(float x, float y) {
        if (stickBase.contains(x, y)) return STICK;
        for (int i = 0; i < buttons.size(); ++i)
            if (buttons.get(i).rect.contains(x, y)) return i;
        return GAME;
    }

    private void haptic() {
        performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
    }

    private void press(Control button, boolean down) {
        if (button.settings) {
            if (down && !settingsHeld) {
                settingsHeld = true;
                SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_F10);
                haptic();
            } else if (!down && settingsHeld) {
                settingsHeld = false;
                SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_F10);
            }
            return;
        }
        if (down) {
            if (button.held++ == 0) {
                if (button.dpad >= 0) TouchInput.setDpad(button.dpad, true);
                else TouchInput.setButton(button.button, true);
                haptic();
            }
        } else if (button.held > 0 && --button.held == 0) {
            if (button.dpad >= 0) TouchInput.setDpad(button.dpad, false);
            else TouchInput.setButton(button.button, false);
        }
    }

    private void setStick(float x, float y) {
        stickX = x;
        stickY = y;
        final float len = (float) Math.hypot(x, y);
        if (len < DEADZONE) {
            TouchInput.setLeftStick(0f, 0f);
            return;
        }
        // clamp to the unit circle and send y up positive
        final float scale = Math.min(1f, len) / len;
        TouchInput.setLeftStick(x * scale, -y * scale);
    }

    private void moveStick(float x, float y) {
        float dx = x - stickBase.centerX();
        float dy = y - stickBase.centerY();
        final float len = (float) Math.hypot(dx, dy);
        if (len > radius) {
            dx = dx / len * radius;
            dy = dy / len * radius;
        }
        setStick(dx / radius, dy / radius);
        stickKnob.set(stickBase.centerX() + dx - radius * 0.42f,
                      stickBase.centerY() + dy - radius * 0.42f,
                      stickBase.centerX() + dx + radius * 0.42f,
                      stickBase.centerY() + dy + radius * 0.42f);
        invalidate();
    }

    private void resetStick() {
        stickPointer = -1;
        stickX = stickY = 0;
        TouchInput.setLeftStick(0f, 0f);
        stickKnob.set(stickBase.centerX() - radius * 0.42f, stickBase.centerY() - radius * 0.42f,
                      stickBase.centerX() + radius * 0.42f, stickBase.centerY() + radius * 0.42f);
        invalidate();
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        if (event.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE) return false;
        final int action = event.getActionMasked();
        final int at = event.getActionIndex();
        if (action == MotionEvent.ACTION_CANCEL) {
            releaseAll();
            return true;
        }
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            final int id = event.getPointerId(at);
            final int owner = hit(event.getX(at), event.getY(at));
            owners.put(id, owner);
            if (owner == STICK) {
                if (stickPointer == -1) {
                    stickPointer = id;
                    moveStick(event.getX(at), event.getY(at));
                }
            } else if (owner >= 0) {
                press(buttons.get(owner), true);
            }
        } else if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < event.getPointerCount(); ++i) {
                final int id = event.getPointerId(i);
                final int owner = owners.get(id, GAME);
                if (id == stickPointer) {
                    moveStick(event.getX(i), event.getY(i));
                } else if (owner >= 0) {
                    final Control button = buttons.get(owner);
                    if (!button.rect.contains(event.getX(i), event.getY(i))) {
                        press(button, false);
                        owners.put(id, GAME);
                    }
                }
            }
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            final int id = event.getPointerId(at);
            final int owner = owners.get(id, GAME);
            if (id == stickPointer) resetStick();
            else if (owner >= 0) press(buttons.get(owner), false);
            owners.delete(id);
            performClick();
        }
        return true;
    }

    void releaseAll() {
        for (Control button : buttons) {
            while (button.held > 0) {
                button.held--;
                if (button.dpad >= 0) TouchInput.setDpad(button.dpad, false);
                else if (!button.settings) TouchInput.setButton(button.button, false);
            }
        }
        if (settingsHeld) {
            settingsHeld = false;
            SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_F10);
        }
        owners.clear();
        resetStick();
        TouchInput.releaseAll();
    }

    @Override protected void onDetachedFromWindow() {
        releaseAll();
        super.onDetachedFromWindow();
    }

    @Override public boolean performClick() { return super.performClick(); }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        paint.setColor(0x55202938);
        canvas.drawCircle(stickBase.centerX(), stickBase.centerY(), radius, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(3f);
        paint.setColor(0x88FFFFFF);
        canvas.drawCircle(stickBase.centerX(), stickBase.centerY(), radius, paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(stickPointer >= 0 ? 0xCC397BC1 : 0xCC4A5A6A);
        canvas.drawRoundRect(stickKnob, radius * 0.42f, radius * 0.42f, paint);
        for (Control button : buttons) {
            paint.setColor(button.held > 0 ? 0xCC397BC1 : 0x88202938);
            canvas.drawRoundRect(button.rect, 14f, 14f, paint);
            paint.setColor(Color.WHITE);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(button.rect.height() * 0.26f);
            canvas.drawText(button.label, button.rect.centerX(),
                button.rect.centerY() - (paint.ascent() + paint.descent()) / 2, paint);
        }
    }
}
