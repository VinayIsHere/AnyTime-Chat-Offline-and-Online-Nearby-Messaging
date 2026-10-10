import { useEffect } from 'react';
import { BrowserRouter, Routes, Route, Navigate, useLocation } from 'react-router-dom';
import { applyThemeMode, getThemeMode } from './theme/theme';
import { useBluetooth } from './hooks/useBluetooth';
import WelcomeScreen from './screens/WelcomeScreen';
import SetupAccountScreen from './screens/SetupAccountScreen';
import BluetoothOffScreen from './screens/BluetoothOffScreen';
import NearbyScreen from './screens/NearbyScreen';
import ChatsScreen from './screens/ChatsScreen';
import ChatScreen from './screens/ChatScreen';
import ProfileScreen from './screens/ProfileScreen';

export default function App() {
  useEffect(() => { applyThemeMode(getThemeMode()); }, []);
  return (
    <BrowserRouter>
      <Routes>
        <Route path="/" element={<Navigate to="/welcome" replace />} />
        <Route path="/welcome" element={<WelcomeScreen />} />
        <Route path="/setup" element={<SetupAccountScreen />} />
        <Route path="/bluetooth" element={<BluetoothOffScreen />} />
        <Route path="/nearby" element={<RequireBluetooth><NearbyScreen /></RequireBluetooth>} />
        <Route path="/chats" element={<RequireBluetooth><ChatsScreen /></RequireBluetooth>} />
        <Route path="/chats/:chatId" element={<RequireBluetooth><ChatScreen /></RequireBluetooth>} />
        <Route path="/profile" element={<RequireBluetooth><ProfileScreen /></RequireBluetooth>} />
      </Routes>
    </BrowserRouter>
  );
}

/**
 * v1 is Bluetooth-only: every in-app route is unusable with the radio off,
 * so the gate redirects rather than overlaying a dismissible banner.
 */
function RequireBluetooth({ children }: { children: React.ReactElement }) {
  const { state, isReady } = useBluetooth();
  const location = useLocation();
  if (state === 'unknown') return null;
  if (!isReady) return <Navigate to="/bluetooth" replace state={{ from: location.pathname }} />;
  return children;
}
