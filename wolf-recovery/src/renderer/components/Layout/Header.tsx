import React, { useEffect, useState } from 'react';
import './Header.css';

interface HeaderProps {
  title: string;
}

function Header({ title }: HeaderProps): React.ReactElement {
  const [isAdmin, setIsAdmin] = useState<boolean>(false);

  useEffect(() => {
    // Check admin status
    if (window.api && window.api.isAdministrator) {
      window.api.isAdministrator()
        .then((result) => {
          setIsAdmin(!!result);
        })
        .catch((err) => {
          console.error('Failed to check admin status:', err);
          setIsAdmin(false);
        });
    }
  }, []);

  const formatTitle = (raw: string) => {
    return raw.charAt(0).toUpperCase() + raw.slice(1);
  };

  return (
    <header className="header">
      <div className="header-title">
        <h2>{formatTitle(title)}</h2>
        {isAdmin ? (
          <span className="badge badge-admin">Administrator</span>
        ) : (
          <span className="badge badge-user">Standard User</span>
        )}
      </div>

      <div className="window-controls">
        {/* TODO: wire up to IPC window controls */}
        <button className="window-control minimize">
          <svg viewBox="0 0 10 1" fill="none" xmlns="http://www.w3.org/2000/svg">
            <line y1="0.5" x2="10" y2="0.5" stroke="currentColor"/>
          </svg>
        </button>
        {/* TODO: wire up to IPC window controls */}
        <button className="window-control maximize">
          <svg viewBox="0 0 10 10" fill="none" xmlns="http://www.w3.org/2000/svg">
            <rect x="0.5" y="0.5" width="9" height="9" stroke="currentColor"/>
          </svg>
        </button>
        {/* TODO: wire up to IPC window controls */}
        <button className="window-control close">
          <svg viewBox="0 0 10 10" fill="none" xmlns="http://www.w3.org/2000/svg">
            <path d="M1 1L9 9M9 1L1 9" stroke="currentColor"/>
          </svg>
        </button>
      </div>
    </header>
  );
}

export default Header;
