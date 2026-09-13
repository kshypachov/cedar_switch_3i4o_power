import { useEffect, useState } from 'react';

/**
 * Three screens do not need a router library. Paths are real URLs - the device
 * answers any path without an extension with the page (SPA fallback) - so a
 * reload or a bookmark lands on the same screen.
 */
export type Route = 'overview' | 'matter' | 'access';

const PATHS: Record<Route, string> = { overview: '/', matter: '/matter', access: '/access' };

export function routeOf(pathname: string): Route {
  if (pathname === PATHS.access) return 'access';
  if (pathname === PATHS.matter) return 'matter';
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
