//===--- CSStep.cpp - Constraint Solver Steps -----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the \c SolverStep class and its related types,
// which is used by constraint solver to do iterative solving.
//
//===----------------------------------------------------------------------===//

#include "CSStep.h"
#include "ConstraintSystem.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"

using namespace llvm;
using namespace swift;
using namespace constraints;

ComponentStep::Scope::Scope(ComponentStep &component)
    : CS(component.CS), Component(component) {
  TypeVars = std::move(CS.TypeVariables);

  for (auto *typeVar : component.TypeVars)
    CS.TypeVariables.push_back(typeVar);

  auto &workList = CS.InactiveConstraints;
  workList.splice(workList.end(), *component.Constraints);

  SolverScope = new ConstraintSystem::SolverScope(CS);
  PrevPartialScope = CS.solverState->PartialSolutionScope;
  CS.solverState->PartialSolutionScope = SolverScope;
}

StepResult SplitterStep::take(bool prevFailed) {
  // "split" is considered a failure if previous step failed,
  // or there is a failure recorded by constraint system, or
  // system can't be simplified.
  if (prevFailed || CS.failedConstraint || CS.simplify())
    return done(/*isSuccess=*/false);

  SmallVector<ComponentStep *, 4> components;
  // Try to run "connected components" algorithm and split
  // type variables and their constraints into independent
  // sub-systems to solve.
  computeFollowupSteps(components);

  // If there is only one component, there is no reason to
  // try to merge solutions, "split" step should be considered
  // done and replaced by a single component step.
  if (components.size() < 2)
    return replaceWith(components.front());

  /// Wait until all of the component steps are done.
  return suspend<ComponentStep>(components);
}

StepResult SplitterStep::resume(bool prevFailed) {
  // Restore the state of the constraint system to before split.
  CS.CG.setOrphanedConstraints(std::move(OrphanedConstraints));
  auto &workList = CS.InactiveConstraints;
  for (auto &component : Components)
    workList.splice(workList.end(), component);

  // If we came back to this step and previous (one of the components)
  // failed, it means that we can't solve this step either.
  if (prevFailed)
    return done(/*isSuccess=*/false);

  // Otherwise let's try to merge partial soltuions together
  // and form a complete solution(s) for this split.
  return done(mergePartialSolutions());
}

void SplitterStep::computeFollowupSteps(
    SmallVectorImpl<ComponentStep *> &componentSteps) {
  // Compute next steps based on that connected components
  // algorithm tells us is splittable.

  auto &CG = CS.getConstraintGraph();
  // Contract the edges of the constraint graph.
  CG.optimize();

  // Compute the connected components of the constraint graph.
  // FIXME: We're seeding typeVars with TypeVariables so that the
  // connected-components algorithm only considers those type variables within
  // our component. There are clearly better ways to do this.
  SmallVector<TypeVariableType *, 16> typeVars(CS.TypeVariables);
  SmallVector<unsigned, 16> components;
  unsigned numComponents = CG.computeConnectedComponents(typeVars, components);
  if (numComponents < 2) {
    componentSteps.push_back(ComponentStep::create(
        CS, 0, /*single=*/true, &CS.InactiveConstraints, Solutions));
    return;
  }

  Components.resize(numComponents);
  PartialSolutions = std::unique_ptr<SmallVector<Solution, 4>[]>(
      new SmallVector<Solution, 4>[numComponents]);

  for (unsigned i = 0, n = numComponents; i != n; ++i) {
    componentSteps.push_back(ComponentStep::create(
        CS, i, /*single=*/false, &Components[i], PartialSolutions[i]));
  }

  if (numComponents > 1 && CS.getASTContext().LangOpts.DebugConstraintSolver) {
    auto &log = CS.getASTContext().TypeCheckerDebug->getStream().indent(
        CS.solverState->depth * 2);

    // Verify that the constraint graph is valid.
    CG.verify();

    log << "---Constraint graph---\n";
    CG.print(log);

    log << "---Connected components---\n";
    CG.printConnectedComponents(log);
  }

  // Map type variables and constraints into appropriate steps.
  llvm::DenseMap<TypeVariableType *, unsigned> typeVarComponent;
  llvm::DenseMap<Constraint *, unsigned> constraintComponent;
  for (unsigned i = 0, n = typeVars.size(); i != n; ++i) {
    auto *typeVar = typeVars[i];
    // Record the component of this type variable.
    typeVarComponent[typeVar] = components[i];

    for (auto *constraint : CG[typeVar].getConstraints())
      constraintComponent[constraint] = components[i];
  }

  // Add the orphaned components to the mapping from constraints to components.
  unsigned firstOrphanedComponent =
      numComponents - CG.getOrphanedConstraints().size();
  {
    unsigned component = firstOrphanedComponent;
    for (auto *constraint : CG.getOrphanedConstraints()) {
      // Register this orphan constraint both as associated with
      // a given component as a regular constrant, as well as an
      // "orphan" constraint, so it can be proccessed correctly.
      constraintComponent[constraint] = component;
      componentSteps[component]->recordOrphan(constraint);
      ++component;
    }
  }

  for (auto *typeVar : CS.TypeVariables) {
    auto known = typeVarComponent.find(typeVar);
    // If current type variable is associated with
    // a certain component step, record it as being so.
    if (known != typeVarComponent.end()) {
      componentSteps[known->second]->record(typeVar);
      continue;
    }

    // Otherwise, associate it with all of the component steps,
    // expect for components with orphaned constraints, they are
    // not supposed to have any type variables.
    for (unsigned i = 0; i != firstOrphanedComponent; ++i)
      componentSteps[i]->record(typeVar);
  }

  // Transfer all of the constraints from the work list to
  // the appropriate component.
  auto &workList = CS.InactiveConstraints;
  while (!workList.empty()) {
    auto *constraint = &workList.front();
    workList.pop_front();
    componentSteps[constraintComponent[constraint]]->record(constraint);
  }

  // Remove all of the orphaned constraints; they'll be re-introduced
  // by each component independently.
  OrphanedConstraints = CG.takeOrphanedConstraints();

  // Create component ordering based on the information associated
  // with constraints in each step - e.g. number of disjunctions,
  // since components are going to be executed in LIFO order, we'd
  // want to have smaller/faster components at the back of the list.
  std::sort(componentSteps.begin(), componentSteps.end(),
            [](const ComponentStep *lhs, const ComponentStep *rhs) {
              return lhs > rhs;
            });
}

bool SplitterStep::mergePartialSolutions() const {
  assert(Components.size() >= 2);

  auto numComponents = Components.size();
  // Produce all combinations of partial solutions.
  SmallVector<unsigned, 2> indices(numComponents, 0);
  bool done = false;
  bool anySolutions = false;
  do {
    // Create a new solver scope in which we apply all of the partial
    // solutions.
    ConstraintSystem::SolverScope scope(CS);
    for (unsigned i = 0; i != numComponents; ++i)
      CS.applySolution(PartialSolutions[i][indices[i]]);

    // This solution might be worse than the best solution found so far.
    // If so, skip it.
    if (!CS.worseThanBestSolution()) {
      // Finalize this solution.
      auto solution = CS.finalize();
      if (CS.TC.getLangOpts().DebugConstraintSolver) {
        auto &log = CS.getASTContext().TypeCheckerDebug->getStream();
        log.indent(CS.solverState->depth * 2)
            << "(composed solution " << CS.CurrentScore << ")\n";
      }

      // Save this solution.
      Solutions.push_back(std::move(solution));
      anySolutions = true;
    }

    // Find the next combination.
    for (unsigned n = numComponents; n > 0; --n) {
      ++indices[n - 1];

      // If we haven't run out of solutions yet, we're done.
      if (indices[n - 1] < PartialSolutions[n - 1].size())
        break;

      // If we ran out of solutions at the first position, we're done.
      if (n == 1) {
        done = true;
        break;
      }

      // Zero out the indices from here to the end.
      for (unsigned i = n - 1; i != numComponents; ++i)
        indices[i] = 0;
    }
  } while (!done);

  return anySolutions;
}

StepResult ComponentStep::take(bool prevFailed) {
  // One of the previous components created by "split"
  // failed, it means that we can't solve this component.
  if (prevFailed)
    return done(/*isSuccess=*/false);

  if (!IsSingle && CS.TC.getLangOpts().DebugConstraintSolver) {
    auto &log = CS.getASTContext().TypeCheckerDebug->getStream();
    log.indent(CS.solverState->depth * 2)
        << "(solving component #" << Index << '\n';
  }

  /// Try to figure out what this step is going to be,
  /// after the scope has been established.
  auto *disjunction = CS.selectDisjunction();
  auto bestBindings = CS.determineBestBindings();

  if (bestBindings && (!disjunction || (!bestBindings->InvolvesTypeVariables &&
                                        !bestBindings->FullyBound))) {
    // Produce a type variable step.
    return suspend(TypeVariableStep::create(CS, *bestBindings, Solutions));
  } else if (disjunction) {
    // Produce a disjunction step.
    return suspend(DisjunctionStep::create(CS, disjunction, Solutions));
  }

  // If there are no disjunctions or type variables to bind
  // we can't solve this system unless we have free type variables
  // allowed in the solution.
  if (!CS.solverState->allowsFreeTypeVariables() && CS.hasFreeTypeVariables())
    return done(/*isSuccess=*/false);

  // If this solution is worse than the best solution we've seen so far,
  // skip it.
  if (CS.worseThanBestSolution())
    return done(/*isSuccess=*/false);

  // If we only have relational or member constraints and are allowing
  // free type variables, save the solution.
  for (auto &constraint : CS.InactiveConstraints) {
    switch (constraint.getClassification()) {
    case ConstraintClassification::Relational:
    case ConstraintClassification::Member:
      continue;
    default:
      return done(/*isSuccess=*/false);
    }
  }

  auto solution = CS.finalize();
  if (CS.TC.getLangOpts().DebugConstraintSolver) {
    auto &log = CS.getASTContext().TypeCheckerDebug->getStream();
    log.indent(CS.solverState->depth * 2)
        << "(found solution " << getCurrentScore() << ")\n";
  }

  Solutions.push_back(std::move(solution));
  return done(/*isSuccess=*/true);
}

StepResult ComponentStep::resume(bool prevFailed) {
  // Rewind all modifications done to constraint system.
  ComponentScope.reset();

  // If we came either back to this step and previous
  // (either disjunction or type var) failed, it means
  // that component as a whole has failed.
  if (prevFailed)
    return done(/*isSuccess=*/false);

  // If this was a single component, there is nothing to be done,
  // because it represents the whole constraint system at some
  // point of the solver path.
  if (IsSingle)
    return done(/*isSuccess=*/true);

  assert(!Solutions.empty() && "No Solutions?");

  // For each of the partial solutions, subtract off the current score.
  // It doesn't contribute.
  for (auto &solution : Solutions)
    solution.getFixedScore() -= OriginalScore;

  // Restore the original best score.
  CS.solverState->BestScore = OriginalBestScore;

  // When there are multiple partial solutions for a given connected component,
  // rank those solutions to pick the best ones. This limits the number of
  // combinations we need to produce; in the common case, down to a single
  // combination.
  filterSolutions(Solutions, /*minimize=*/true);
  return done(/*isSuccess=*/true);
}

void TypeVariableStep::setup() {
  auto &TC = CS.TC;
  ++CS.solverState->NumTypeVariablesBound;
  if (TC.getLangOpts().DebugConstraintSolver) {
    auto &log = TC.Context.TypeCheckerDebug->getStream();
    log.indent(CS.solverState->depth * 2) << "Initial bindings: ";
    interleave(InitialBindings.begin(), InitialBindings.end(),
               [&](const Binding &binding) {
                 log << TypeVar->getString()
                     << " := " << binding.BindingType->getString();
               },
               [&log] { log << ", "; });

    log << '\n';
  }
}

StepResult TypeVariableStep::take(bool prevFailed) {
  auto &TC = CS.TC;
  while (auto binding = Producer()) {
    // Try each of the bindings in turn.
    ++CS.solverState->NumTypeVariableBindings;

    if (AnySolved) {
      // If this is a defaultable binding and we have found solutions,
      // don't explore the default binding.
      if (binding->isDefaultable())
        continue;

      // If we were able to solve this without considering
      // default literals, don't bother looking at default literals.
      if (binding->hasDefaultedProtocol() && !SawFirstLiteralConstraint)
        break;
    }

    if (TC.getLangOpts().DebugConstraintSolver) {
      auto &log = TC.Context.TypeCheckerDebug->getStream();
      log.indent(CS.solverState->depth * 2) << "(trying ";
      binding->print(log, &TC.Context.SourceMgr);
      log << '\n';
    }

    if (binding->hasDefaultedProtocol())
      SawFirstLiteralConstraint = true;

    {
      // Try to solve the system with typeVar := type
      auto scope = llvm::make_unique<Scope>(CS);
      if (binding->attempt(CS)) {
        ActiveChoice = std::move(scope);
        // Looks like binding attempt has been successful,
        // let's try to see if it leads to any solutions.
        return suspend(SplitterStep::create(CS, Solutions));
      }
    }
  }

  // No more bindings to try, or producer has been short-circuited.
  return done(/*isSuccess=*/AnySolved);
}

StepResult TypeVariableStep::resume(bool prevFailed) {
  assert(ActiveChoice);

  // Rewind back all of the changes made to constraint system.
  ActiveChoice.reset();

  // If there was no failure in the sub-path it means
  // that active binding has a solution.
  AnySolved |= !prevFailed;

  auto &TC = CS.TC;
  if (TC.getLangOpts().DebugConstraintSolver) {
    auto &log = TC.Context.TypeCheckerDebug->getStream();
    log.indent(CS.solverState->depth * 2) << ")\n";
  }

  // If there has been at least one solution so far
  // at a current batch of bindings is done it's a
  // success because each new batch would be less
  // and less precise.
  if (AnySolved && Producer.needsToComputeNext())
    return done(/*isSuccess=*/true);

  // Attempt next type variable binding.
  return take(prevFailed);
}

StepResult DisjunctionStep::take(bool prevFailed) {
  while (auto binding = Producer()) {
    auto &currentChoice = *binding;

    if (shouldSkipChoice(currentChoice))
      continue;

    if (shouldShortCircuitAt(currentChoice))
      break;

    if (CS.TC.getLangOpts().DebugConstraintSolver) {
      auto &ctx = CS.getASTContext();
      auto &log = ctx.TypeCheckerDebug->getStream();
      log.indent(CS.solverState->depth) << "(assuming ";
      currentChoice.print(log, &ctx.SourceMgr);
      log << '\n';
    }

    /// Attempt given disjunction choice, which is going to simplify
    /// constraint system by binding some of the type variables. Since
    /// the system has been simplified and is splittable, we simplify
    /// have to return "split" step which is going to take care of the rest.
    if (attemptChoice(currentChoice))
      return suspend(SplitterStep::create(CS, Solutions));
  }

  return done(/*isSuccess=*/bool(LastSolvedChoice));
}

StepResult DisjunctionStep::resume(bool prevFailed) {
  // If disjunction step is re-taken and there should be
  // active choice, let's see if it has be solved or not.
  assert(ActiveChoice);

  if (CS.TC.getLangOpts().DebugConstraintSolver) {
    auto &log = CS.getASTContext().TypeCheckerDebug->getStream();
    log.indent(CS.solverState->depth) << ")\n";
  }

  // If choice (sub-path) has failed, it's okay, other
  // choices have to be attempted regardless, since final
  // decision could be made only after attempting all
  // of the choices, so let's just ignore failed ones.
  if (!prevFailed) {
    auto &choice = ActiveChoice->second;
    auto score = getBestScore(Solutions);

    if (!choice.isGenericOperator() && choice.isSymmetricOperator()) {
      if (!BestNonGenericScore || score < BestNonGenericScore)
        BestNonGenericScore = score;
    }

    // Remember the last successfully solved choice,
    // it would be useful when disjunction is exhausted.
    LastSolvedChoice = {choice, *score};
  }

  // Rewind back the constraint system information.
  ActiveChoice.reset();

  // Attempt next disjunction choice (if any left).
  return take(prevFailed);
}

bool DisjunctionStep::shouldSkipChoice(const TypeBinding &choice) const {
  auto &TC = CS.TC;

  if (choice.isDisabled()) {
    if (TC.getLangOpts().DebugConstraintSolver) {
      auto &log = CS.getASTContext().TypeCheckerDebug->getStream();
      log.indent(CS.solverState->depth) << "(skipping ";
      choice.print(log, &TC.Context.SourceMgr);
      log << '\n';
    }

    return true;
  }

  // Skip unavailable overloads unless solver is in the "diagnostic" mode.
  if (!CS.shouldAttemptFixes() && choice.isUnavailable())
    return true;

  if (TC.getLangOpts().DisableConstraintSolverPerformanceHacks)
    return false;

  // Don't attempt to solve for generic operators if we already have
  // a non-generic solution.

  // FIXME: Less-horrible but still horrible hack to attempt to
  //        speed things up. Skip the generic operators if we
  //        already have a solution involving non-generic operators,
  //        but continue looking for a better non-generic operator
  //        solution.
  if (BestNonGenericScore && choice.isGenericOperator()) {
    auto &score = BestNonGenericScore->Data;
    // Let's skip generic overload choices only in case if
    // non-generic score indicates that there were no forced
    // unwrappings of optional(s), no unavailable overload
    // choices present in the solution, no fixes required,
    // and there are no non-trivial function conversions.
    if (score[SK_ForceUnchecked] == 0 && score[SK_Unavailable] == 0 &&
        score[SK_Fix] == 0 && score[SK_FunctionConversion] == 0)
      return true;
  }

  return false;
}

bool DisjunctionStep::shouldShortCircuitAt(
    const DisjunctionChoice &choice) const {
  if (!LastSolvedChoice)
    return false;

  auto *lastChoice = LastSolvedChoice->first;
  auto delta = LastSolvedChoice->second - getCurrentScore();
  bool hasUnavailableOverloads = delta.Data[SK_Unavailable] > 0;
  bool hasFixes = delta.Data[SK_Fix] > 0;

  // Attempt to short-circuit evaluation of this disjunction only
  // if the disjunction choice we are comparing to did not involve
  // selecting unavailable overloads or result in fixes being
  // applied to reach a solution.
  return !hasUnavailableOverloads && !hasFixes &&
         shortCircuitDisjunctionAt(choice, lastChoice);
}

bool DisjunctionStep::shortCircuitDisjunctionAt(
    Constraint *currentChoice, Constraint *lastSuccessfulChoice) const {
  auto &ctx = CS.getASTContext();
  if (ctx.LangOpts.DisableConstraintSolverPerformanceHacks)
    return false;

  // If the successfully applied constraint is favored, we'll consider that to
  // be the "best".
  if (lastSuccessfulChoice->isFavored() && !currentChoice->isFavored()) {
#if !defined(NDEBUG)
    if (lastSuccessfulChoice->getKind() == ConstraintKind::BindOverload) {
      auto overloadChoice = lastSuccessfulChoice->getOverloadChoice();
      assert((!overloadChoice.isDecl() ||
              !overloadChoice.getDecl()->getAttrs().isUnavailable(ctx)) &&
             "Unavailable decl should not be favored!");
    }
#endif

    return true;
  }

  // Anything without a fix is better than anything with a fix.
  if (currentChoice->getFix() && !lastSuccessfulChoice->getFix())
    return true;

  if (auto restriction = currentChoice->getRestriction()) {
    // Non-optional conversions are better than optional-to-optional
    // conversions.
    if (*restriction == ConversionRestrictionKind::OptionalToOptional)
      return true;

    // Array-to-pointer conversions are better than inout-to-pointer
    // conversions.
    if (auto successfulRestriction = lastSuccessfulChoice->getRestriction()) {
      if (*successfulRestriction == ConversionRestrictionKind::ArrayToPointer &&
          *restriction == ConversionRestrictionKind::InoutToPointer)
        return true;
    }
  }

  // Implicit conversions are better than checked casts.
  if (currentChoice->getKind() == ConstraintKind::CheckedCast)
    return true;

  return false;
}

bool DisjunctionStep::attemptChoice(const DisjunctionChoice &choice) {
  auto scope = llvm::make_unique<Scope>(CS);
  ++CS.solverState->NumDisjunctionTerms;

  // If the disjunction requested us to, remember which choice we
  // took for it.
  if (auto *disjunctionLocator = Producer.getLocator()) {
    auto index = choice.getIndex();
    CS.DisjunctionChoices.push_back({disjunctionLocator, index});

    // Implicit unwraps of optionals are worse solutions than those
    // not involving implicit unwraps.
    if (!disjunctionLocator->getPath().empty()) {
      auto kind = disjunctionLocator->getPath().back().getKind();
      if (kind == ConstraintLocator::ImplicitlyUnwrappedDisjunctionChoice ||
          kind == ConstraintLocator::DynamicLookupResult) {
        assert(index == 0 || index == 1);
        if (index == 1)
          CS.increaseScore(SK_ForceUnchecked);
      }
    }
  }

  if (!choice.attempt(CS))
    return false;

  // Establish the "active" choice which maintains new scope in the
  // constraint system, be be able to rollback all of the changes later.
  ActiveChoice.emplace(std::move(scope), choice);
  return true;
}
