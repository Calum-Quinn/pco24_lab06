<div align="justify" style="margin-right:25px;margin-left:25px">

# Laboratoire 6 : Thread Pool <!-- omit from toc -->

## Etudiants <!-- omit from toc -->

- Calum Quinn
- Urs Behrmann

# Table des matières

- [Table des matières](#table-des-matières)
- [Introduction](#introduction)
- [Conception](#conception)
  - [Choix d'implémentation](#choix-dimplémentation)
- [Tests](#tests)
- [Conclusion](#conclusion)

# Introduction

Le but primaire de ce laboratoire est d'implémenter un thread pool dynamic. Celui-ci doit pouvoir prendre n'importe quel type de tâche et pouvoir gérer de façon efficace le nombre de threads actifs selon le taux de travail actuel.

# Conception



## Choix d'implémentation

### Gestion des timeouts

Un point crucial de ce projet consiste en la suppression des threads qui deviennent inactifs pendant un certain temps.

#### Thread de gestion

Pour pouvoir gérer efficacement les threads qui pourraient dépasser le temps de timeout, nous avons créés un thread séparé qui sert à gérer le calcul du temps et la suppression des threads.

#### Queue de valeurs temporels

Pour que le thread de gestion puisse savoir s'il faut retirer un thread, il doit avoir accès au temps d'attente de chaque thread.
Nous avons choisi d'utiliser une `std::queue` pour stocker les valeurs de temps car nous n'avons donc pas besoin de faire de recherche à l'intérieur.
Ceci car une queue à la propriété FIFO intégré, nous pouvons donc savoir que la première valeur retirée était la première mise et représente donc le temps d'attente le plus long.

A l'intérieur de cette queue nous n'avons pas mis simplement un timestamp sinon nous ne pourrions pas retirer le thread qui a dépassé le timeout.
Nous avons donc décidé d'y mettre des `std::pair` pour lier le timestamp au thread qui devra potentiellement être retiré.
Nous n'avons pas utiliser une map car celui-ci manque la propriété FIFO et nécessiterait donc une recherche à travers la structure entière pour savoir si un thread a dépassé le timemout.

#### Bloquage du destructeur

Dans le cadre d'une application concurrente, la gestion de la terminaison des threads peut être complexe.

Afin d'éviter la tentative d'une double terminaison de threads, nous avons utilisés les variables `cleaning` et `cleaner`.
Ceux-ci servent à montrer qu'un thread est actuellement en phase de terminaison après avoir dépassé le timeout.
Sans ces valeurs il serait possible que le destructeur essaie de retirer le même thread que le thread de management.

# Tests



# Conclusion



</div>