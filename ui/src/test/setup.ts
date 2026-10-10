import '@testing-library/jest-dom/vitest';
import { cleanup } from '@testing-library/react';
import { afterEach } from 'vitest';

afterEach(cleanup);

// jsdom implements window.scrollTo but not Element.scrollTo, and ChatScreen
// scrolls its thread to the newest message on every render.
if (!Element.prototype.scrollTo) {
  Element.prototype.scrollTo = () => {};
}

// Nor blob URLs, which ChatScreen creates for photo previews and revokes when
// the chat closes.
if (!URL.createObjectURL) URL.createObjectURL = () => 'blob:test';
if (!URL.revokeObjectURL) URL.revokeObjectURL = () => {};
