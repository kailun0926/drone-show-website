import { Component, signal } from '@angular/core';
import { FormsModule } from '@angular/forms';

interface Member {
  name: string;
  role: string;
  icon: string;
  photo?: string;
}

@Component({
  selector: 'app-team',
  imports: [FormsModule],
  templateUrl: './team.html',
  styleUrl: './team.scss',
})
export class Team {
  protected readonly members: Member[] = [
    { name: 'Liang-Kai Wang', role: 'Project Manager', icon: '🧭' },
    { name: 'Kai-Lun Chen', role: 'Software Engineer', icon: '💻', photo: 'kailun.jpg' },
    { name: 'Chia-Chien Lin', role: 'Cloud Technician', icon: '☁️', photo: 'milu.jpg' },
    { name: 'Shao-Yu Liu', role: 'Hardware Engineer', icon: '🔧' },
  ];

  protected readonly contact = { name: '', email: '', message: '' };
  protected readonly submitted = signal(false);

  submit(): void {
    // Static-site demo: wire up Formspree / an n8n Webhook for real submissions
    this.submitted.set(true);
  }
}
