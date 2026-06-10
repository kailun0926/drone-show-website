import { Routes } from '@angular/router';
import { Home } from './pages/home/home';
import { Concept } from './pages/concept/concept';
import { Future } from './pages/future/future';
import { Team } from './pages/team/team';

export const routes: Routes = [
  { path: '', component: Home, title: 'Drone Show | Swarm Control System' },
  { path: 'concept', component: Concept, title: 'Concept & Technology | Drone Show' },
  { path: 'future', component: Future, title: 'Future Outlook | Drone Show' },
  { path: 'team', component: Team, title: 'Team & Contact | Drone Show' },
  { path: '**', redirectTo: '' },
];
