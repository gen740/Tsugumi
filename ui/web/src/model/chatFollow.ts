// Whether the chat keeps showing its newest message. Only the reader leaves the
// end, by scrolling up: content growing never does, however much it adds at once.

/** This close to the end (px) counts as at the end. */
export const CHAT_END_MARGIN = 32

export interface ChatScrollState {
  top: number
  height: number
  viewport: number
}

export function chatFollowsAfterScroll(following: boolean, lastTop: number, now: ChatScrollState): boolean {
  if (now.height - now.viewport - now.top <= CHAT_END_MARGIN) return true
  return now.top < lastTop ? false : following
}
