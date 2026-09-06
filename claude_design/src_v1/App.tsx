import { useEffect } from 'react';
import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom';
import { applyThemeMode, getThemeMode } from './theme/theme';
import WelcomeScreen from './screens/WelcomeScreen';
import SetupAccountScreen from './screens/SetupAccountScreen';
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
        <Route path="/nearby" element={<NearbyScreen />} />
        <Route path="/chats" element={<ChatsScreen />} />
        <Route path="/chats/:chatId" element={<ChatScreen />} />
        <Route path="/profile" element={<ProfileScreen />} />
      </Routes>
    </BrowserRouter>
  );
}
