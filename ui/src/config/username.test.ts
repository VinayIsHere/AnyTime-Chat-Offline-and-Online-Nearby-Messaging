import { describe, expect, it } from 'vitest';

import { isValidUsername, MAX_USERNAME, MIN_USERNAME, sanitizeUsername } from './username';

// These mirror AccountService::isValidUsername. If one side changes, the core
// suite and this one should both be updated in the same commit.
describe('username rules', () => {
  it('accepts the documented alphabet at both length limits', () => {
    expect(isValidUsername('a'.repeat(MIN_USERNAME))).toBe(true);
    expect(isValidUsername('a'.repeat(MAX_USERNAME))).toBe(true);
    expect(isValidUsername('vinay_k_99')).toBe(true);
  });

  it('refuses anything the core refuses', () => {
    expect(isValidUsername('a'.repeat(MIN_USERNAME - 1))).toBe(false);
    expect(isValidUsername('a'.repeat(MAX_USERNAME + 1))).toBe(false);
    expect(isValidUsername('Vinay')).toBe(false);
    expect(isValidUsername('vi nay')).toBe(false);
    expect(isValidUsername('vinay!')).toBe(false);
    expect(isValidUsername('vinay·4f2a')).toBe(false);
    expect(isValidUsername('')).toBe(false);
  });

  it('sanitises typing into something valid-shaped', () => {
    expect(sanitizeUsername('Vinay K!')).toBe('vinayk');
    expect(sanitizeUsername('a'.repeat(30))).toHaveLength(MAX_USERNAME);
    expect(sanitizeUsername('@priya_s')).toBe('priya_s');
  });
});
