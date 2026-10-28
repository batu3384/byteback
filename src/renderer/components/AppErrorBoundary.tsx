import React from 'react'
import { appErrorBoundaryCopy } from '../../shared/app-error-boundary-message'

interface Props {
  children: React.ReactNode
  scanId: number
  scanLive: boolean
}

interface State {
  error: Error | null
}

/** Catches render errors so one broken panel does not blank the whole examiner UI. */
export default class AppErrorBoundary extends React.Component<Props, State> {
  state: State = { error: null }

  static getDerivedStateFromError(error: Error): State {
    return { error }
  }

  componentDidCatch(error: Error, info: React.ErrorInfo): void {
    console.error('[renderer] uncaught render error:', error, info.componentStack)
    const stack = info.componentStack ?? ''
    void window.api?.logRendererError?.(error.message, stack)
  }

  private handlePrimary = (): void => {
    const copy = appErrorBoundaryCopy(this.props.scanLive, this.props.scanId)
    if (copy.useWindowReload) {
      window.location.reload()
      return
    }
    this.setState({ error: null })
  }

  render(): React.ReactNode {
    if (this.state.error) {
      const copy = appErrorBoundaryCopy(this.props.scanLive, this.props.scanId)
      return (
        <div className="app-error-boundary" role="alert">
          <h1>UI error</h1>
          <p>{copy.body}</p>
          <button type="button" onClick={this.handlePrimary}>
            {copy.primaryLabel}
          </button>
        </div>
      )
    }
    return this.props.children
  }
}
