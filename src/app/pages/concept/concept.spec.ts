import { ComponentFixture, TestBed } from '@angular/core/testing';

import { Concept } from './concept';

describe('Concept', () => {
  let component: Concept;
  let fixture: ComponentFixture<Concept>;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      imports: [Concept]
    })
    .compileComponents();

    fixture = TestBed.createComponent(Concept);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });
});
