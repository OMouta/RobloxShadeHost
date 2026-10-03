// How many questions someone outside the team can ask within a minute.
export const maxQuestions = 4;
const windowMs = 60_000;

// When each person's recent questions came in, and who has been told to slow down since their last answer.
const recent = new Map<string, number[]>();
const warned = new Set<string>();

// "answer" while the person is under the limit. Over it, "warn" the first time and "ignore" after that, so someone
// who keeps going gets one reply and not one per message.
export function limit(userId: string, now = Date.now()): "answer" | "warn" | "ignore" {
  const times = (recent.get(userId) ?? []).filter((time) => now - time < windowMs);
  if (times.length < maxQuestions) {
    recent.set(userId, [...times, now]);
    warned.delete(userId);
    return "answer";
  }
  recent.set(userId, times);
  if (warned.has(userId)) return "ignore";
  warned.add(userId);
  return "warn";
}
