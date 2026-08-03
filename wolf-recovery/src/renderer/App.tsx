import React, { useState } from 'react'
import Sidebar from './components/Layout/Sidebar'
import Header from './components/Layout/Header'
import Dashboard from './components/Dashboard/Dashboard'

type Page = 'dashboard' | 'scan' | 'results' | 'hex' | 'imager' | 'smart'

function App(): React.ReactElement {
  const [activePage, setActivePage] = useState<Page>('dashboard')

  const renderPage = () => {
    switch (activePage) {
      case 'dashboard': return <Dashboard />
      default: return (
        <div className="placeholder-page">
          <h2>{activePage.toUpperCase()}</h2>
          <p>Coming in Phase 2</p>
        </div>
      )
    }
  }

  return (
    <div className="app-layout">
      <Sidebar activePage={activePage} onNavigate={setActivePage} />
      <div className="app-main">
        <Header title={activePage} />
        <main className="app-content">
          {renderPage()}
        </main>
      </div>
    </div>
  )
}

export default App
