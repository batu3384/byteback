// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from 'vitest'
import { cleanup, fireEvent, render, screen } from '@testing-library/react'
import React from 'react'
import AppErrorBoundary from './AppErrorBoundary'

afterEach(cleanup)

function Boom(): React.ReactElement {
  throw new Error('boom')
}

describe('AppErrorBoundary', () => {
  it('shows fallback without leaking exception text', () => {
    const spy = vi.spyOn(console, 'error').mockImplementation(() => {})
    render(
      <AppErrorBoundary scanId={3} scanLive={false}>
        <Boom />
      </AppErrorBoundary>,
    )
    expect(screen.getByRole('alert')).toBeTruthy()
    expect(screen.queryByText('boom')).toBeNull()
    expect(screen.getByText(/Last scan id on disk: 3/)).toBeTruthy()
    spy.mockRestore()
  })

  it('Try again clears the error state', () => {
    const spy = vi.spyOn(console, 'error').mockImplementation(() => {})
    let fail = true
    function MaybeBoom(): React.ReactElement {
      if (fail) throw new Error('once')
      return <p>ok</p>
    }
    render(
      <AppErrorBoundary scanId={-1} scanLive={false}>
        <MaybeBoom />
      </AppErrorBoundary>,
    )
    fail = false
    fireEvent.click(screen.getByRole('button', { name: 'Try again' }))
    expect(screen.getByText('ok')).toBeTruthy()
    spy.mockRestore()
  })

  it('logs to session via IPC when available', () => {
    const spy = vi.spyOn(console, 'error').mockImplementation(() => {})
    const logRendererError = vi.fn()
    window.api = { ...window.api, logRendererError } as typeof window.api
    render(
      <AppErrorBoundary scanId={1} scanLive={true}>
        <Boom />
      </AppErrorBoundary>,
    )
    expect(logRendererError).toHaveBeenCalled()
    spy.mockRestore()
  })
})
