# Sub-project 2: window system + desktop shell — direction note

Not a spec yet. This records the intent so it is not lost, and so the kernel
work now does not paint us into a corner. A real spec gets written when the
kernel core is done.

## The look: a mini Windows computer

The desktop should read as a tiny desktop PC, not as an embedded menu. Chrome
that says "computer" rather than "gadget":

- A **taskbar** along the bottom with a start button, running-app buttons, and
  a clock in the corner.
- **Windows with title bars** — title text, and minimise/maximise/close boxes
  at the right.
- A **start menu** that pops up from the button, listing apps.
- **Desktop icons** on a patterned background.
- Beveled, high-contrast widgets — the 3D-edge look reads as "PC" at very small
  sizes, and it survives a 240x135 screen far better than flat design does,
  because a one-pixel light/dark edge is legible where a subtle fill is not.

The joke and the appeal are the same thing: it is a whole desktop computer on a
device the size of a credit card, and it should look like one.

## What the hardware allows, and what that forces

240x135 is the entire desktop. Some arithmetic that constrains the design:

- At the console's 6x8 font the screen is 40x16 characters. A taskbar 10 pixels
  tall costs a row and leaves 125 pixels for windows.
- A title bar of 9 pixels plus a 1-pixel border means a window costs ~12
  pixels of height before any content. Two stacked windows and a taskbar leave
  very little; **overlapping windows, not tiling**, is the only thing that
  makes sense here.
- A full-screen 16-bit canvas is 65 KB, a fifth of the heap. **There cannot be
  a back buffer per window.** The compositor has to repaint damaged rectangles
  straight to the panel, the way the console already repaints single cells.
  This is the single biggest constraint on the design and it should be settled
  before anything is drawn.
- Window contents therefore need to be redrawable on demand — an expose/paint
  callback per window rather than a retained bitmap.

## What it needs from the kernel

- **The context switch**, so each window's app is a task. Currently blocking
  sub-project 1.
- **Damage-rectangle tracking** and a z-ordered window list, which is
  bookkeeping over small structs and so is host-testable, like the scheduler's
  ready queue. That is where the tests should go: overlap, occlusion, and
  damage merging are exactly the sort of geometry that is wrong in the corners.
- **Mouse input** eventually (sub-project 5, Bluetooth HID), but the keyboard
  has to drive it first: Fn plus the `; , . /` arrow cluster to move focus,
  and something for window switching.

## Open questions for the real spec

- Is the start menu a window, or special-cased chrome?
- Does an app own its window's full content area, or is there a widget layer?
- What happens when a window is larger than the screen — scroll, or forbid?
- Where does the existing 40x16 console live: as a window ("Terminal"), or
  does it stay the pre-desktop boot environment? Probably both.
