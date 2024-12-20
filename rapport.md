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

Le but primaire de ce laboratoire est d'implémenter un thread pool. Celui-ci doit pouvoir prendre n'importe quel type de tâche et pouvoir gérer de façon efficace le nombre de threads actifs selon le taux de travail actuel.

# Conception



## Choix d'implémentation

### Gestion des timeouts

#### Thread de gestion

Thread séparé pour pouvoir gérer les timeouts et retirer les threads idle

#### Queue de valeurs temporels

Pourquoi une queue de pairs (FIFO) donc pas une Map

#### Bloquage du destructeur

Pourquoi cleaning et cleaner pour bloquer

# Tests



# Conclusion



</div>