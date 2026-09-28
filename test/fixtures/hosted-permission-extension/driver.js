globalThis.hostedPermissionAudit = {
  result: { state: 'idle' },
  request() {
    if (this.result.state !== 'idle') throw new Error('request already started')
    this.requestInfo = {
      runtimeId: chrome.runtime.id,
      url: location.href,
      visibility: document.visibilityState,
      focused: document.hasFocus(),
      userActivation: {
        isActive: navigator.userActivation.isActive,
        hasBeenActive: navigator.userActivation.hasBeenActive,
      },
    }
    this.result = { state: 'pending' }
    try {
      chrome.permissions.request({ permissions: ['tabs'] }).then(
        granted => { this.result = { state: 'resolved', granted } },
        error => { this.result = { state: 'rejected', error: String(error) } },
      )
    } catch (error) {
      this.result = { state: 'rejected', error: String(error) }
    }
    return this.result
  },
}
