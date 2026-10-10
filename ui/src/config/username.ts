/**
 * Username rules, shared by sign-up and the Profile editor so the two cannot
 * drift apart.
 *
 * Mirrors AccountService::isValidUsername and core/domain/constants/
 * RequestConstants.h — keep the three in step. The core is the authority and
 * refuses anything else; checking here only saves a round trip and lets the
 * field explain itself before the user presses Save.
 *
 * AD-05: format only, never uniqueness. There is no server to ask.
 */

export const MIN_USERNAME = 3;
export const MAX_USERNAME = 20;

const USERNAME_RE = new RegExp(`^[a-z0-9_]{${MIN_USERNAME},${MAX_USERNAME}}$`);

export const USERNAME_HINT = `${MIN_USERNAME}–${MAX_USERNAME} characters — letters, numbers and _`;

export function isValidUsername(value: string): boolean {
  return USERNAME_RE.test(value);
}

/**
 * What a keystroke may leave in the field: lowercase, allowed characters only,
 * capped at the maximum. Applied on every change, so the field can never hold
 * something the core would refuse for a reason the user cannot see.
 */
export function sanitizeUsername(input: string): string {
  return input.toLowerCase().replace(/[^a-z0-9_]/g, '').slice(0, MAX_USERNAME);
}
