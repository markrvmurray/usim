/* pv_psram.h
 *
 * The simulator's stand-in for the firmware's PSRAM probe VERDICT. On the
 * board, every PSRAM-backed store -- the graphics shadow, the get/put buffer
 * pool, the command trace, the flood fill's span log and seed stack, and the
 * parked windows -- asks one question, win_store_slots() == 0, and returns
 * NULL when the part is absent. Each caller has a fallback for that. Without
 * this switch the shims always hand out arrays, so no test had ever run any
 * of those fallbacks, although every image ships them.
 *
 * One switch rather than one per store, because the firmware takes one
 * verdict and the stores share it: "the shadow is missing but the pool is
 * not" is a state the board cannot be in, and testing it would prove
 * nothing. Flip it only between tests, after a link reset: state resolved
 * against storage that then vanishes (a selected pattern or font) is a
 * mid-run hot-unplug, which the board cannot do either.
 */

#pragma once

void pv_psram_set_absent(bool absent);
