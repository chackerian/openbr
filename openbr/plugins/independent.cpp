#include <QFutureSynchronizer>
#include <QtConcurrentRun>

#include "openbr_internal.h"
#include "openbr/core/common.h"

using namespace cv;

namespace br
{

static TemplateList Downsample(const TemplateList &templates, int classes, int instances, float fraction, const QString & inputVariable)
{
    // Return early when no downsampling is required
    if ((classes == std::numeric_limits<int>::max()) &&
            (instances == std::numeric_limits<int>::max()) &&
            (fraction >= 1))
        return templates;

    const bool atLeast = instances < 0;
    instances = abs(instances);

    QList<QString> allLabels = File::get<QString>(templates, inputVariable);
    QList<QString> uniqueLabels = allLabels.toSet().toList();
    qSort(uniqueLabels);

    QMap<QString,int> counts = templates.countValues<QString>(inputVariable, instances != std::numeric_limits<int>::max());

    if ((instances != std::numeric_limits<int>::max()) && (classes != std::numeric_limits<int>::max()))
        foreach (const QString & label, counts.keys())
            if (counts[label] < instances)
                counts.remove(label);

    uniqueLabels = counts.keys();
    if ((classes != std::numeric_limits<int>::max()) && (uniqueLabels.size() < classes))
        qWarning("Downsample requested %d classes but only %d are available.", classes, uniqueLabels.size());

    QList<QString> selectedLabels = uniqueLabels;
    if (classes < uniqueLabels.size()) {
        std::random_shuffle(selectedLabels.begin(), selectedLabels.end());
        selectedLabels = selectedLabels.mid(0, classes);
    }

    TemplateList downsample;
    for (int i=0; i<selectedLabels.size(); i++) {
        const QString selectedLabel = selectedLabels[i];
        QList<int> indices;
        for (int j=0; j<allLabels.size(); j++)
            if ((allLabels[j] == selectedLabel) && (!templates.value(j).file.get<bool>("FTE", false)))
                indices.append(j);

        std::random_shuffle(indices.begin(), indices.end());
        const int max = atLeast ? indices.size() : std::min(indices.size(), instances);
        for (int j=0; j<max; j++)
            downsample.append(templates.value(indices[j]));
    }

    if (fraction < 1) {
        std::random_shuffle(downsample.begin(), downsample.end());
        downsample = downsample.mid(0, downsample.size()*fraction);
    }

    return downsample;
}

class DownsampleTrainingTransform : public Transform
{
    Q_OBJECT
    Q_PROPERTY(br::Transform* transform READ get_transform WRITE set_transform RESET reset_transform STORED true)
    Q_PROPERTY(int classes READ get_classes WRITE set_classes RESET reset_classes STORED false)
    Q_PROPERTY(int instances READ get_instances WRITE set_instances RESET reset_instances STORED false)
    Q_PROPERTY(float fraction READ get_fraction WRITE set_fraction RESET reset_fraction STORED false)
    Q_PROPERTY(QString inputVariable READ get_inputVariable WRITE set_inputVariable RESET reset_inputVariable STORED false)
    BR_PROPERTY(br::Transform*, transform, NULL)
    BR_PROPERTY(int, classes, std::numeric_limits<int>::max())
    BR_PROPERTY(int, instances, std::numeric_limits<int>::max())
    BR_PROPERTY(float, fraction, 1)
    BR_PROPERTY(QString, inputVariable, "Label")

    void project(const Template & src, Template & dst) const
    {
       transform->project(src,dst);      
    }
  
	  
    void train(const TemplateList &data)
    {
        if (!transform || !transform->trainable)
            return;

        TemplateList downsampled = Downsample(data, classes, instances, fraction, inputVariable);
        transform->train(downsampled);
    }
};
BR_REGISTER(Transform, DownsampleTrainingTransform)

/*!
 * \ingroup transforms
 * \brief Clones the transform so that it can be applied independently.
 * \author Josh Klontz \cite jklontz
 * \em Independent transforms expect single-matrix templates.
 */
class IndependentTransform : public MetaTransform
{
    Q_OBJECT
    Q_PROPERTY(br::Transform* transform READ get_transform WRITE set_transform RESET reset_transform STORED false)
    BR_PROPERTY(br::Transform*, transform, NULL)

    QList<Transform*> transforms;

    void init()
    {
        transforms.clear();
        if (transform == NULL)
            return;

        transform->setParent(this);
        transforms.append(transform);
        file = transform->file;
        trainable = transform->trainable;
        setObjectName(transform->objectName());
    }

    Transform *clone() const
    {
        IndependentTransform *independentTransform = new IndependentTransform();
        independentTransform->transform = transform->clone();
        independentTransform->init();
        return independentTransform;
    }

    static void _train(Transform *transform, const TemplateList *data)
    {
        transform->train(*data);
    }

    void train(const TemplateList &data)
    {
        // Don't bother if the transform is untrainable
        if (!trainable) return;

        QList<TemplateList> templatesList;
        foreach (const Template &t, data) {
            if ((templatesList.size() != t.size()) && !templatesList.isEmpty())
                qWarning("Independent::train (%s) template %s of size %d differs from expected size %d.", qPrintable(objectName()), qPrintable(t.file.name), t.size(), templatesList.size());
            while (templatesList.size() < t.size())
                templatesList.append(TemplateList());
            for (int i=0; i<t.size(); i++)
                templatesList[i].append(Template(t.file, t[i]));
        }

        while (transforms.size() < templatesList.size())
            transforms.append(transform->clone());

        QFutureSynchronizer<void> futures;
        for (int i=0; i<templatesList.size(); i++)
	  futures.addFuture(QtConcurrent::run(_train, transforms[i], &templatesList[i]));
	futures.waitForFinished();
    }

    void project(const Template &src, Template &dst) const
    {
        dst.file = src.file;
        QList<Mat> mats;
        for (int i=0; i<src.size(); i++) {
            transforms[i%transforms.size()]->project(Template(src.file, src[i]), dst);
            mats.append(dst);
            dst.clear();
        }
        dst.append(mats);
    }

    void store(QDataStream &stream) const
    {
        const int size = transforms.size();
        stream << size;
        for (int i=0; i<size; i++)
            transforms[i]->store(stream);
    }

    void load(QDataStream &stream)
    {
        int size;
        stream >> size;
        while (transforms.size() < size)
            transforms.append(transform->clone());
        for (int i=0; i<size; i++)
            transforms[i]->load(stream);
    }
};

BR_REGISTER(Transform, IndependentTransform)

/*!
 * \ingroup transforms
 * \brief A globally shared transform.
 * \author Josh Klontz \cite jklontz
 */
class SingletonTransform : public MetaTransform
{
    Q_OBJECT
    Q_PROPERTY(QString description READ get_description WRITE set_description RESET reset_description STORED false)
    BR_PROPERTY(QString, description, "Identity")

    static QMutex mutex;
    static QHash<QString,Transform*> transforms;
    static QHash<QString,int> trainingReferenceCounts;
    static QHash<QString,TemplateList> trainingData;

    Transform *transform;

    void init()
    {
        QMutexLocker locker(&mutex);
        if (!transforms.contains(description)) {
            transforms.insert(description, make(description));
            trainingReferenceCounts.insert(description, 0);
        }

        transform = transforms[description];
        trainingReferenceCounts[description]++;
    }

    void train(const TemplateList &data)
    {
        QMutexLocker locker(&mutex);
        trainingData[description].append(data);
        trainingReferenceCounts[description]--;
        if (trainingReferenceCounts[description] > 0) return;
        transform->train(trainingData[description]);
        trainingData[description].clear();
    }

    void project(const Template &src, Template &dst) const
    {
        transform->project(src, dst);
    }

    void store(QDataStream &stream) const
    {
        if (transform->parent() == this)
            transform->store(stream);
    }

    void load(QDataStream &stream)
    {
        if (transform->parent() == this)
            transform->load(stream);
    }
};

QMutex SingletonTransform::mutex;
QHash<QString,Transform*> SingletonTransform::transforms;
QHash<QString,int> SingletonTransform::trainingReferenceCounts;
QHash<QString,TemplateList> SingletonTransform::trainingData;

BR_REGISTER(Transform, SingletonTransform)

} // namespace br

#include "independent.moc"
