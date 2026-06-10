import { Component } from '@angular/core';

interface Milestone {
  phase: string;
  title: string;
  description: string;
  tags: string[];
}

@Component({
  selector: 'app-future',
  imports: [],
  templateUrl: './future.html',
  styleUrl: './future.scss',
})
export class Future {
  protected readonly milestones: Milestone[] = [
    {
      phase: 'Now',
      title: 'Current Architecture: ESP-NOW Broadcasting',
      description:
        'A three-level Edge / Fog / Cloud architecture centered on low-latency ESP-NOW broadcasting, with sensor data collection, Zone Position Map computation, and MQTT-based Global AP Map updates already in place.',
      tags: ['ESP-NOW', 'MQTT', 'Edge / Fog'],
    },
    {
      phase: 'Next',
      title: 'Architecture Upgrade: Multi-Agent Systems (MAS)',
      description:
        'Introduce Multi-Agent Systems optimization algorithms so drones can perform more sophisticated dynamic obstacle avoidance and autonomous formation flying based on Goal Programming — evolving from passively receiving commands to autonomous cooperative decision-making.',
      tags: ['Multi-Agent Systems', 'Goal Programming', 'Obstacle Avoidance', 'Auto Formation'],
    },
    {
      phase: 'Future',
      title: 'Industrial-Grade Monitoring: n8n + LLM Smart Operations',
      description:
        'Combine n8n Webhooks with LLMs: when a sensor such as the MPU6500 detects abnormal vibration or attitude deviation, an alert workflow is triggered automatically and a diagnostic report is generated — raising system reliability to industrial grade.',
      tags: ['n8n Webhook', 'LLM', 'Anomaly Detection', 'Auto Diagnostics'],
    },
  ];
}
