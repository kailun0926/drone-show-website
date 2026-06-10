import { ComponentFixture, TestBed } from '@angular/core/testing';

import { Future } from './future';

describe('Future', () => {
  let component: Future;
  let fixture: ComponentFixture<Future>;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      imports: [Future]
    })
    .compileComponents();

    fixture = TestBed.createComponent(Future);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });
});
