import React, { useEffect, useState } from 'react'

function App(): React.ReactElement {
  const [version, setVersion] = useState('')
  const [isAdmin, setIsAdmin] = useState(false)

  useEffect(() => {
    window.api.getEngineVersion().then(setVersion).catch(console.error)
    window.api.isAdministrator().then(setIsAdmin).catch(console.error)
  }, [])

  return (
    <div className="app">
      <h1>🐺 Wolf Recovery</h1>
      <p>Engine v{version} | Admin: {isAdmin ? '✅' : '❌'}</p>
    </div>
  )
}

export default App
