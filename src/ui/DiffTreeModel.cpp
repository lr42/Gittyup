//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "DiffTreeModel.h"
#include "conf/Settings.h"
#include "git/Blob.h"
#include "git/Diff.h"
#include "git/RevWalk.h"
#include "git/Submodule.h"
#include <QStringBuilder>
#include <QUrl>

namespace {

const QString kLinkFmt = "<a href='%1'>%2</a>";

} // anon. namespace

DiffTreeModel::DiffTreeModel(const git::Repository &repo, QObject *parent)
  : QAbstractItemModel(parent), mRepo(repo)
{}

DiffTreeModel::~DiffTreeModel()
{
  delete mRoot;
}

void DiffTreeModel::createDiffTree()
{

    for (int i = 0; i < mDiff.count(); ++i) {
        QString path = mDiff.name(i);
        auto pathParts = path.split("/");
        mRoot->addChild(pathParts, 0);
    }
}

void DiffTreeModel::setDiff(const git::Diff &diff)
{
  beginResetModel();

  if (diff) {
      delete mRoot;
      mDiff = diff;
      mRoot = new Node(mRepo.workdir().path());
      createDiffTree();
  }

  endResetModel();
}

int DiffTreeModel::rowCount(const QModelIndex &parent) const
{
  return mDiff ? node(parent)->children().size() : 0;
}

int DiffTreeModel::columnCount(const QModelIndex &parent) const
{
  return 1;
}

bool DiffTreeModel::hasChildren(const QModelIndex &parent) const
{
  return mRoot && node(parent)->hasChildren();
}

QModelIndex DiffTreeModel::parent(const QModelIndex &index) const
{
  Node *parent = node(index)->parent();
  if (!parent || parent == mRoot)
    return QModelIndex();

  Q_ASSERT(parent->parent()); // because parent is not root
  return createIndex(parent->parent()->children().indexOf(parent), 0, parent);
}

QModelIndex DiffTreeModel::index(
  int row,
  int column,
  const QModelIndex &parent) const
{
  if (row < 0 || row >= rowCount(parent) ||
      column < 0 || column >= columnCount(parent))
    return QModelIndex();

  return createIndex(row, column, node(parent)->children().at(row));
}

QVariant DiffTreeModel::data(const QModelIndex &index, int role) const
{
  if (!index.isValid())
    return QVariant();

  Node *node = this->node(index);
  switch (role) {
    case Qt::DisplayRole:
      return node->name();

//    case Qt::DecorationRole: {
//      QFileInfo info(node->path());
//      return info.exists() ? mIconProvindexider.icon(info) :
//        mIconProvider.icon(QFileIconProvider::File);
//    }

    case Qt::EditRole:
      return node->path(true);

    case Qt::ToolTipRole:
      return node->path();

    case Qt::CheckStateRole: {
      if (!mDiff.isValid() || !mDiff.isStatusDiff())
        return QVariant();

      QStringList paths;
      QString prefix = node->path(true);
	  // from these files the checkstate for the folders is created,
	  // because the folder it self cannot be staged
      for (int i = 0; i < mDiff.count(); ++i) {
        QString path = mDiff.name(i);
        if (path.startsWith(prefix))
          paths.append(path);
      }

      if (paths.isEmpty())
        return QVariant();

      int count = 0;
      git::Index index = mDiff.index();
      foreach (const QString &path, paths) {
		// isStaged on folders does not work, because folder cannot be staged
        switch (index.isStaged(path)) {
          case git::Index::Disabled:
          case git::Index::Unstaged:
          case git::Index::Conflicted:
            break;

          case git::Index::PartiallyStaged:
            return Qt::PartiallyChecked;
          case git::Index::Staged:
            ++count;
            break;
        }
      }

      if (count == 0) {
        return Qt::Unchecked;
      } else if (count == paths.size()) {
        return Qt::Checked;
      } else {
        return Qt::PartiallyChecked;
      }
    }

    case KindRole: {
      Settings *settings = Settings::instance();
      git::Submodule submodule = mRepo.lookupSubmodule(node->path(true));
      return submodule.isValid() ? tr("Submodule") : settings->kind(node->name());
    }

    case AddedRole:
    case ModifiedRole: {
      int sort = GIT_SORT_TIME;
      if (role == AddedRole)
        sort |= GIT_SORT_REVERSE;
      git::RevWalk walker = mRepo.walker(sort);
      git::Commit commit = walker.next(node->path(true));
      if (!commit.isValid())
        return QVariant();

      QUrl url;
      url.setScheme("id");
      url.setPath(commit.id().toString());
      return kLinkFmt.arg(url.toString(), commit.shortId());
    }

    case StatusRole: {
      if (!mDiff.isValid())
        return QString();

      QString status;
      QString prefix = node->path(true);
      for (int i = 0; i < mDiff.count(); ++i) {
        QString name = mDiff.name(i);
        if (containsPath(name, prefix)) {
          QChar ch = git::Diff::statusChar(mDiff.status(i));
          if (!status.contains(ch))
            status.append(ch);
        }
      }

      return status;
    }
  }

  return QVariant();
}

bool DiffTreeModel::setData(const QModelIndex &index,
                        const QVariant &value,
                        int role)
{
    return setData(index, value, role, false);
}

bool DiffTreeModel::setData(const QModelIndex &index,
                        const QVariant &value,
                        int role,
                        bool ignoreIndexChanges)
{
  switch (role) {
    case Qt::CheckStateRole: {
      QStringList files;
      Node *node = this->node(index);
      QString prefix = node->path(true);
      for (int i = 0; i < mDiff.count(); ++i) {
        QString file = mDiff.name(i);
        if (file.startsWith(prefix))
          files.append(file);
      }

      if (!ignoreIndexChanges)
        mDiff.index().setStaged(files, value.toBool());

	  // childs
	  if (hasChildren(index)) {
		  // emit dataChanged() for all files in the folder
		  // all children changed too. TODO: only the tracked files should emit a signal
		  int count = rowCount(index);
		  for (int row = 0; row < count; row++) {
			  QModelIndex child = this->index(row, 0, index);
			  emit dataChanged(child, child, {role});
		  }
	  }
	  // parents
	  // recursive approach to emit signal dataChanged also for the parents.
	  // Because when a file in a folder is staged, the state of the folder changes too
	  QModelIndex parent = this->parent(index);
	  while (parent.isValid()) {
		  emit dataChanged(parent, parent, {role});
		  parent = this->parent(parent);
	  }

	  // file/folder it self
	  // emit dataChanged() for folder or file it self
	  emit dataChanged(index, index, {role});
      emit checkStateChanged(index, value.toInt());

      return true;
    }
  }

  return false;
}

Qt::ItemFlags DiffTreeModel::flags(const QModelIndex &index) const
{
  return QAbstractItemModel::flags(index) | Qt::ItemIsUserCheckable;
}

DiffTreeModel::Node *DiffTreeModel::node(const QModelIndex &index) const
{
  return index.isValid() ? static_cast<Node *>(index.internalPointer()) : mRoot;
}

//#############################################################################
//######     DiffTreeModel::Node     ##############################################
//#############################################################################

DiffTreeModel::Node::Node(const QString &name, Node *parent)
  : mName(name), mParent(parent)
{}

DiffTreeModel::Node::~Node()
{
  qDeleteAll(mChildren);
}

QString DiffTreeModel::Node::name() const
{
  return mName;
}

QString DiffTreeModel::Node::path(bool relative) const
{
  bool root = (!mParent || (relative && !mParent->mParent));
  return !root ? mParent->path(relative) % "/" % mName : mName;
}

DiffTreeModel::Node *DiffTreeModel::Node::parent() const
{
  return mParent;
}

bool DiffTreeModel::Node::hasChildren() const
{
  return mChildren.length() > 0;
}

QList<DiffTreeModel::Node *> DiffTreeModel::Node::children()
{
  return mChildren;
}

void DiffTreeModel::Node::addChild(const QStringList& pathPart, int indexFirstDifferent)
{
    for (auto c: mChildren) {
        if (c->name() == pathPart[indexFirstDifferent]) {
            c->addChild(pathPart, indexFirstDifferent + 1);
            return;
        }
    }

    auto n = new Node(pathPart[indexFirstDifferent], this);
    if (indexFirstDifferent + 1 < pathPart.length()) {
        n->addChild(pathPart, indexFirstDifferent + 1);
    }
    mChildren.append(n);
}
