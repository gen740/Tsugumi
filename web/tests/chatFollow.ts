// The chat stays at its newest message until the reader scrolls up, and comes
// back when the reader returns to the end.
import assert from 'node:assert/strict'
import { CHAT_END_MARGIN, chatFollowsAfterScroll } from '../src/model/chatFollow'

const viewport = 500

// Scrolling up leaves the end, and growth while reading does not bring it back.
assert.equal(chatFollowsAfterScroll(true, 1500, { top: 1200, height: 2000, viewport }), false)
assert.equal(chatFollowsAfterScroll(false, 1200, { top: 1200, height: 9000, viewport }), false)

// A scroll event read after the content grew: the position did not go up, so
// the reader did not leave (the next resize takes it to the new end).
assert.equal(chatFollowsAfterScroll(true, 1500, { top: 1500, height: 9000, viewport }), true)

// Scrolling down short of the end stays away; reaching the end follows again.
assert.equal(chatFollowsAfterScroll(false, 1200, { top: 1300, height: 2000, viewport }), false)
assert.equal(chatFollowsAfterScroll(false, 1300, { top: 1500, height: 2000, viewport }), true)

// A small movement at the end is still at the end, up to the margin.
assert.equal(chatFollowsAfterScroll(true, 1500, { top: 1500 - CHAT_END_MARGIN, height: 2000, viewport }), true)
assert.equal(chatFollowsAfterScroll(true, 1500, { top: 1500 - CHAT_END_MARGIN - 1, height: 2000, viewport }), false)

// A conversation shorter than its area is always at the end (a new conversation empties it).
assert.equal(chatFollowsAfterScroll(false, 1500, { top: 0, height: 100, viewport }), true)

console.log('chat follow behavior: passed')
