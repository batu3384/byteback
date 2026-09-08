// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from 'vitest'
import { cleanup, fireEvent, render } from '@testing-library/react'
import ConfirmModal from './ConfirmModal'

// vitest globals are off, so @testing-library/react's automatic cleanup does
// not register — unmounting here also deregisters each modal from the
// module-level Escape stack between tests.
afterEach(cleanup)

function renderModal(open: boolean, onCancel = vi.fn(), onConfirm = vi.fn()) {
  return render(
    <ConfirmModal
      open={open}
      title="Test title"
      body="Test body"
      confirmLabel="Confirm"
      onCancel={onCancel}
      onConfirm={onConfirm}
    />,
  )
}

describe('ConfirmModal', () => {
  it('Escape triggers onCancel exactly once per event', () => {
    const onCancel = vi.fn()
    renderModal(true, onCancel)
    fireEvent.keyDown(window, { key: 'Escape' })
    expect(onCancel).toHaveBeenCalledTimes(1)
  })

  it('Tab cycles inside the trap — focus does not escape to document.body', () => {
    const { getByTestId } = renderModal(true)
    const accept = getByTestId('confirm-modal-accept')
    accept.focus()
    expect(document.activeElement).toBe(accept)
    // Accept is the last focusable; Tab must wrap to the first (cancel), not
    // leave the dialog.
    fireEvent.keyDown(window, { key: 'Tab' })
    expect(document.activeElement).not.toBe(document.body)
    expect((document.activeElement as HTMLElement).tagName).toBe('BUTTON')
    // Shift+Tab from the first focusable wraps back to accept.
    fireEvent.keyDown(window, { key: 'Tab', shiftKey: true })
    expect(document.activeElement).toBe(accept)
  })

  it('backdrop click closes (click on the dialog overlay, not the panel)', () => {
    const onCancel = vi.fn()
    const { getByTestId, getByText } = renderModal(true, onCancel)
    fireEvent.click(getByTestId('confirm-modal'))
    expect(onCancel).toHaveBeenCalledTimes(1)
    // A click inside the panel must not bubble into a cancel.
    fireEvent.click(getByText('Test body'))
    expect(onCancel).toHaveBeenCalledTimes(1)
  })

  it('focus is restored to the trigger on close', () => {
    const trigger = document.createElement('button')
    trigger.textContent = 'trigger'
    document.body.appendChild(trigger)
    try {
      trigger.focus()
      const { rerender, getByTestId } = render(
        <ConfirmModal open={false} title="T" body="B" confirmLabel="C" onCancel={() => {}} onConfirm={() => {}} />,
      )
      rerender(
        <ConfirmModal open title="T" body="B" confirmLabel="C" onCancel={() => {}} onConfirm={() => {}} />,
      )
      expect(document.activeElement).toBe(getByTestId('confirm-modal-accept'))
      rerender(
        <ConfirmModal open={false} title="T" body="B" confirmLabel="C" onCancel={() => {}} onConfirm={() => {}} />,
      )
      expect(document.activeElement).toBe(trigger)
    } finally {
      trigger.remove()
    }
  })

  it('with two stacked modals only the topmost receives Escape', () => {
    const bottomCancel = vi.fn()
    const topCancel = vi.fn()
    render(
      <ConfirmModal open title="Bottom" body="B" confirmLabel="C" onCancel={bottomCancel} onConfirm={() => {}} />,
    )
    // Mounted after Bottom, so Top registers last and sits on top of the stack.
    render(
      <ConfirmModal open title="Top" body="B" confirmLabel="C" onCancel={topCancel} onConfirm={() => {}} />,
    )
    fireEvent.keyDown(window, { key: 'Escape' })
    expect(topCancel).toHaveBeenCalledTimes(1)
    expect(bottomCancel).not.toHaveBeenCalled()
    // Closing the top modal (unmount via cancel in real usage) hands Escape
    // back to the bottom modal.
    cleanup()
    render(
      <ConfirmModal open title="Bottom" body="B" confirmLabel="C" onCancel={bottomCancel} onConfirm={() => {}} />,
    )
    fireEvent.keyDown(window, { key: 'Escape' })
    expect(bottomCancel).toHaveBeenCalledTimes(1)
  })
})
