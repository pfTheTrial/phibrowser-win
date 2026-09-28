'use strict';

// In-memory ownership only: no extension/profile storage is read or written.
const ownedWindows = new Set();
globalThis.hostedShellAudit = {
  async create(options) {
    const window = await chrome.windows.create({
      ...options,
      url: chrome.runtime.getURL('marker.html') + '?run=' +
          encodeURIComponent(location.search),
    });
    if (!Number.isInteger(window?.id)) throw new Error('create returned no window ID');
    ownedWindows.add(window.id);
    return window;
  },
  get: (id) => chrome.windows.get(id),
  list: () => chrome.windows.getAll(),
  async focus(id) {
    if (!ownedWindows.has(id)) throw new Error('Refusing to focus unowned window');
    return chrome.windows.update(id, { focused: true });
  },
  async remove(id) {
    if (!ownedWindows.has(id)) throw new Error('Refusing to remove unowned window');
    await chrome.windows.remove(id);
    ownedWindows.delete(id);
  },
  owned: () => [...ownedWindows],
  async cleanup() {
    const errors = [];
    for (const id of [...ownedWindows].reverse()) {
      try {
        await chrome.windows.remove(id);
        ownedWindows.delete(id);
      } catch (error) {
        // Already closed is success, but retain IDs for other failures.
        const stillExists = (await chrome.windows.getAll()).some(w => w.id === id);
        if (stillExists) errors.push({ id, error: String(error) });
        else ownedWindows.delete(id);
      }
    }
    return { remaining: [...ownedWindows], errors };
  },
};
