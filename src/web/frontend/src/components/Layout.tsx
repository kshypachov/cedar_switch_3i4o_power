import type { ReactNode } from 'react';

import { logout } from '../api/auth';
import type { Session } from '../api/types';
import { t } from '../i18n';
import { useAuth } from '../state/auth';
import { hrefOf, navigate, type Route } from '../state/router';
import { HttpWarning } from './ui';

function NavLink({
  route,
  current,
  label,
}: {
  route: Route;
  current: Route;
  label: 'nav.overview' | 'nav.matter' | 'nav.network' | 'nav.logs' | 'nav.coprocessor' | 'nav.access';
}) {
  return (
    <a
      href={hrefOf(route)}
      aria-current={route === current ? 'page' : undefined}
      onClick={(e) => {
        e.preventDefault();
        navigate(route);
      }}
    >
      {t(label)}
    </a>
  );
}

export function Layout({ session, route, children }: { session: Session; route: Route; children: ReactNode }) {
  const { signedOut } = useAuth();
  return (
    <>
      <header className="topbar">
        <span className="brand">{t('app.title')}</span>
        <nav>
          <NavLink route="overview" current={route} label="nav.overview" />
          <NavLink route="matter" current={route} label="nav.matter" />
          <NavLink route="network" current={route} label="nav.network" />
          <NavLink route="logs" current={route} label="nav.logs" />
          <NavLink route="coprocessor" current={route} label="nav.coprocessor" />
          <NavLink route="access" current={route} label="nav.access" />
        </nav>
        <button
          type="button"
          className="button-secondary"
          onClick={async () => {
            try {
              await logout(session.csrf_token);
            } finally {
              signedOut(null);
            }
          }}
        >
          {t('nav.logout')}
        </button>
      </header>
      {children}
      <footer>
        <HttpWarning />
      </footer>
    </>
  );
}
