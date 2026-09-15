import { useEffect, useState } from 'react';

/**
 * A few screens do not need a router library. Paths are real URLs - the device
 * answers any path without an extension with the page (SPA fallback) - so a
 * reload or a bookmark lands on the same screen. The query string is the
 * screen's own (`/network?txn=`); the route is the path alone.
 */
export type Route = 'overview' | 'matter' | 'network' | 'logs' | 'coprocessor' | 'firmware' | 'access';

const PATHS: Record<Route, string> = {
  overview: '/',
  matter: '/matter',
  network: '/network',
  logs: '/logs',
  coprocessor: '/coprocessor',
  firmware: '/firmware',
  access: '/access',
};

export function routeOf(pathname: string): Route {
  if (pathname === PATHS.access) return 'access';
  if (pathname === PATHS.matter) return 'matter';
  if (pathname === PATHS.network) return 'network';
  if (pathname === PATHS.logs) return 'logs';
  if (pathname === PATHS.coprocessor) return 'coprocessor';
  if (pathname === PATHS.firmware) return 'firmware';
  return 'overview';
}

export function navigate(route: Route): void {
  if (window.location.pathname !== PATHS[route]) {
    window.history.pushState(null, '', PATHS[route]);
    window.dispatchEvent(new PopStateEvent('popstate'));
  }
}

export function hrefOf(route: Route): string {
  return PATHS[route];
}

export function useRoute(): Route {
  const [route, setRoute] = useState<Route>(() => routeOf(window.location.pathname));
  useEffect(() => {
    const onPop = () => setRoute(routeOf(window.location.pathname));
    window.addEventListener('popstate', onPop);
    return () => window.removeEventListener('popstate', onPop);
  }, []);
  return route;
}
