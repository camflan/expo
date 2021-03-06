/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ABI38_0_0ShadowTree.h"

#include <ABI38_0_0React/components/root/RootComponentDescriptor.h>
#include <ABI38_0_0React/components/view/ViewShadowNode.h>
#include <ABI38_0_0React/core/LayoutContext.h>
#include <ABI38_0_0React/core/LayoutPrimitives.h>
#include <ABI38_0_0React/debug/SystraceSection.h>
#include <ABI38_0_0React/mounting/MountingTelemetry.h>
#include <ABI38_0_0React/mounting/ShadowTreeRevision.h>
#include <ABI38_0_0React/mounting/ShadowViewMutation.h>

#include "ABI38_0_0ShadowTreeDelegate.h"

namespace ABI38_0_0facebook {
namespace ABI38_0_0React {

static void CommitState(ShadowNode::Shared const &shadowNode) {
  auto state = shadowNode->getState();
  if (state) {
    state->commit(shadowNode);
  }
}

static void updateMountedFlag(
    const SharedShadowNodeList &oldChildren,
    const SharedShadowNodeList &newChildren) {
  // This is a simplified version of Diffing algorithm that only updates
  // `mounted` flag on `ShadowNode`s. The algorithm sets "mounted" flag before
  // "unmounted" to allow `ShadowNode` detect a situation where the node was
  // remounted.

  if (&oldChildren == &newChildren) {
    // Lists are identical, nothing to do.
    return;
  }

  if (oldChildren.size() == 0 && newChildren.size() == 0) {
    // Both lists are empty, nothing to do.
    return;
  }

  int index;

  // Stage 1: Mount and unmount "updated" children.
  for (index = 0; index < oldChildren.size() && index < newChildren.size();
       index++) {
    const auto &oldChild = oldChildren[index];
    const auto &newChild = newChildren[index];

    if (oldChild == newChild) {
      // Nodes are identical, skipping the subtree.
      continue;
    }

    if (!ShadowNode::sameFamily(*oldChild, *newChild)) {
      // Totally different nodes, updating is impossible.
      break;
    }

    newChild->setMounted(true);
    CommitState(newChild);
    oldChild->setMounted(false);

    updateMountedFlag(oldChild->getChildren(), newChild->getChildren());
  }

  int lastIndexAfterFirstStage = index;

  // State 2: Mount new children.
  for (index = lastIndexAfterFirstStage; index < newChildren.size(); index++) {
    const auto &newChild = newChildren[index];
    newChild->setMounted(true);
    CommitState(newChild);
    updateMountedFlag({}, newChild->getChildren());
  }

  // State 3: Unmount old children.
  for (index = lastIndexAfterFirstStage; index < oldChildren.size(); index++) {
    const auto &oldChild = oldChildren[index];
    oldChild->setMounted(false);
    updateMountedFlag(oldChild->getChildren(), {});
  }
}

ShadowTree::ShadowTree(
    SurfaceId surfaceId,
    LayoutConstraints const &layoutConstraints,
    LayoutContext const &layoutContext,
    RootComponentDescriptor const &rootComponentDescriptor,
    ShadowTreeDelegate const &delegate)
    : surfaceId_(surfaceId), delegate_(delegate) {
  const auto noopEventEmitter = std::make_shared<const ViewEventEmitter>(
      nullptr, -1, std::shared_ptr<const EventDispatcher>());

  const auto props = std::make_shared<const RootProps>(
      *RootShadowNode::defaultSharedProps(), layoutConstraints, layoutContext);

  rootShadowNode_ = std::static_pointer_cast<const RootShadowNode>(
      rootComponentDescriptor.createShadowNode(ShadowNodeFragment{
          /* .tag = */ surfaceId,
          /* .surfaceId = */ surfaceId,
          /* .props = */ props,
          /* .eventEmitter = */ noopEventEmitter,
      }));

  mountingCoordinator_ = std::make_shared<MountingCoordinator const>(
      ShadowTreeRevision{rootShadowNode_, 0, {}});
}

ShadowTree::~ShadowTree() {
  mountingCoordinator_->revoke();
}

Tag ShadowTree::getSurfaceId() const {
  return surfaceId_;
}

void ShadowTree::commit(ShadowTreeCommitTransaction transaction) const {
  SystraceSection s("ShadowTree::commit");

  int attempts = 0;

  while (true) {
    attempts++;
    if (tryCommit(transaction)) {
      return;
    }

    // After multiple attempts, we failed to commit the transaction.
    // Something internally went terribly wrong.
    assert(attempts < 1024);
  }
}

bool ShadowTree::tryCommit(ShadowTreeCommitTransaction transaction) const {
  SystraceSection s("ShadowTree::tryCommit");

  auto telemetry = MountingTelemetry{};
  telemetry.willCommit();

  RootShadowNode::Shared oldRootShadowNode;

  {
    // Reading `rootShadowNode_` in shared manner.
    std::shared_lock<better::shared_mutex> lock(commitMutex_);
    oldRootShadowNode = rootShadowNode_;
  }

  RootShadowNode::Unshared newRootShadowNode = transaction(oldRootShadowNode);

  if (!newRootShadowNode) {
    return false;
  }

  std::vector<LayoutableShadowNode const *> affectedLayoutableNodes{};
  affectedLayoutableNodes.reserve(1024);

  telemetry.willLayout();
  newRootShadowNode->layout(&affectedLayoutableNodes);
  telemetry.didLayout();

  newRootShadowNode->sealRecursive();

  auto revisionNumber = ShadowTreeRevision::Number{};

  {
    // Updating `rootShadowNode_` in unique manner if it hasn't changed.
    std::unique_lock<better::shared_mutex> lock(commitMutex_);

    if (rootShadowNode_ != oldRootShadowNode) {
      return false;
    }

    rootShadowNode_ = newRootShadowNode;

    {
      std::lock_guard<std::mutex> dispatchLock(EventEmitter::DispatchMutex());

      updateMountedFlag(
          oldRootShadowNode->getChildren(), newRootShadowNode->getChildren());
    }

    revisionNumber_++;
    revisionNumber = revisionNumber_;
  }

  emitLayoutEvents(affectedLayoutableNodes);

  telemetry.didCommit();

  mountingCoordinator_->push(
      ShadowTreeRevision{newRootShadowNode, revisionNumber, telemetry});

  delegate_.shadowTreeDidFinishTransaction(*this, mountingCoordinator_);

  return true;
}

void ShadowTree::commitEmptyTree() const {
  commit(
      [](RootShadowNode::Shared const &oldRootShadowNode)
          -> RootShadowNode::Unshared {
        return std::make_shared<RootShadowNode>(
            *oldRootShadowNode,
            ShadowNodeFragment{
                /* .tag = */ ShadowNodeFragment::tagPlaceholder(),
                /* .surfaceId = */ ShadowNodeFragment::surfaceIdPlaceholder(),
                /* .props = */ ShadowNodeFragment::propsPlaceholder(),
                /* .eventEmitter = */
                ShadowNodeFragment::eventEmitterPlaceholder(),
                /* .children = */ ShadowNode::emptySharedShadowNodeSharedList(),
            });
      });
}

void ShadowTree::emitLayoutEvents(
    std::vector<LayoutableShadowNode const *> &affectedLayoutableNodes) const {
  SystraceSection s("ShadowTree::emitLayoutEvents");

  for (auto const *layoutableNode : affectedLayoutableNodes) {
    // Only instances of `ViewShadowNode` (and subclasses) are supported.
    auto const &viewShadowNode =
        static_cast<ViewShadowNode const &>(*layoutableNode);
    auto const &viewEventEmitter = static_cast<ViewEventEmitter const &>(
        *viewShadowNode.getEventEmitter());

    // Checking if the `onLayout` event was requested for the particular Shadow
    // Node.
    auto const &viewProps =
        static_cast<ViewProps const &>(*viewShadowNode.getProps());
    if (!viewProps.onLayout) {
      continue;
    }

    viewEventEmitter.onLayout(layoutableNode->getLayoutMetrics());
  }
}

} // namespace ABI38_0_0React
} // namespace ABI38_0_0facebook
