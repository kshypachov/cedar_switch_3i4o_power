import { ErrorNotice } from './components/ErrorNotice';
import { Layout } from './components/Layout';
import { Loading } from './components/ui';
import { AccessScreen } from './features/auth/AccessScreen';
import { LoginScreen } from './features/auth/LoginScreen';
import { SetupScreen } from './features/auth/SetupScreen';
import { CoprocessorUpdateScreen } from './features/coprocessor-update/CoprocessorUpdateScreen';
import { OverviewScreen } from './features/device/OverviewScreen';
import { LogsScreen } from './features/logs/LogsScreen';
import { MatterScreen } from './features/matter/MatterScreen';
import { NetworkScreen } from './features/network/NetworkScreen';
import { SystemUpdateScreen } from './features/system-update/SystemUpdateScreen';
import { t } from './i18n';
import { AuthProvider, useAuth } from './state/auth';
import { useRoute } from './state/router';

function Shell() {
  const { view, refresh } = useAuth();
  const route = useRoute();

  switch (view.kind) {
    case 'loading':
      return (
        <main className="narrow">
          <Loading />
        </main>
      );
    case 'unreachable':
      return (
        <main className="narrow">
          <ErrorNotice error={view.error} onRetry={() => void refresh()} />
        </main>
      );
    case 'setup':
      return <SetupScreen state={view.state} />;
    case 'login':
      return <LoginScreen notice={view.notice} />;
    case 'locked':
      return (
        <main className="narrow">
          <p className="notice notice-error">{t('setup.unavailable')}</p>
        </main>
      );
    case 'signed_in':
      return (
        <Layout session={view.session} route={route}>
          {route === 'access' ? (
            <AccessScreen session={view.session} />
          ) : route === 'matter' ? (
            <MatterScreen session={view.session} />
          ) : route === 'network' ? (
            <NetworkScreen session={view.session} />
          ) : route === 'logs' ? (
            <LogsScreen />
          ) : route === 'coprocessor' ? (
            <CoprocessorUpdateScreen session={view.session} />
          ) : route === 'firmware' ? (
            <SystemUpdateScreen session={view.session} />
          ) : (
            <OverviewScreen />
          )}
        </Layout>
      );
  }
}

export function App() {
  return (
    <AuthProvider>
      <Shell />
    </AuthProvider>
  );
}
