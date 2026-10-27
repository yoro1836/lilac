// Maven Central answers 404/429 at random through this container's proxy; Google's GCS mirror of it does not
val mirror = "https://maven-central.storage-download.googleapis.com/maven2/"
fun fix(r: ArtifactRepository) {
    if (r is MavenArtifactRepository && r.url.toString().contains("repo.maven.apache.org")) r.setUrl(mirror)
}
fun RepositoryHandler.mirrorFirst() {
    all { fix(this) }
    if (findByName("centralMirror") == null) {
        val m = maven { url = uri(mirror); name = "centralMirror" }
        remove(m)
        addFirst(m)
    }
}
beforeSettings {
    pluginManagement.repositories.mirrorFirst()
    dependencyResolutionManagement.repositories.mirrorFirst()
    buildscript.repositories.mirrorFirst()
}
allprojects {
    buildscript.repositories.all { fix(this) }
    repositories.all { fix(this) }
}
